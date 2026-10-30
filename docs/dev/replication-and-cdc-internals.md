# Replication and change feed internals

The change feed (`CDC.*`) and replication are two readers of the same thing: the RocksDB write-ahead log, kept
after recovery no longer needs it. This page covers WAL retention, the `RocksStorage` WAL APIs, the change notes
that make WAL batches decodable, the CDC decoder and its blocking reads, and the replication protocol with its
primary and replica sides. User-facing behavior is in [change-feed.md](../change-feed.md) and
[replication.md](../replication.md).

## Contents

- [Overview](#overview)
- [WAL retention](#wal-retention)
- [RocksStorage WAL APIs](#rocksstorage-wal-apis)
- [Change notes](#change-notes)
- [Change feed decoding](#change-feed-decoding)
- [CDC.READ and BLOCK](#cdcread-and-block)
- [Replication protocol](#replication-protocol)
- [Primary side: serve_replica](#primary-side-serve_replica)
- [Replica side: ReplicaLink](#replica-side-replicalink)
- [Applying batches on the replica](#applying-batches-on-the-replica)
- [Read-only replicas](#read-only-replicas)
- [Promotion](#promotion)
- [Known limitations](#known-limitations)
- [Tests](#tests)

## Overview

```text
            write batch (+ change note)
 Database ─────────────────────────────▶ RocksDB WAL ──(archived, kept for wal-retention)
                                             │
              ┌──────────────────────────────┼─────────────────────────────┐
              ▼                              ▼                             ▼
   read_wal + WalVisitor           WalReader (raw batches)         on_next_write (wake-ups)
   change_feed.cpp: Decoder        replica_link.cpp: stream_wal    asio_util.h: wait_for_write
   CDC.READ                        REPLSYNC stream                 CDC.READ BLOCK, stream_wal
```

Every committed write batch has a RocksDB sequence number (the sequence of its first operation; the next batch
starts at `seq + count`). CDC cursors and replication offsets are such sequence numbers, always pointing at the
start of a batch: "the next sequence to read".

## WAL retention

Configuration (see [configuration.md](../configuration.md)):

| Setting | `Config` | `StorageOptions` | RocksDB |
|---|---|---|---|
| `wal-retention` (default 0, else at least 1 s) | `wal_retention` (ms) | `wal_retention_s = ms / 1000` | `WAL_ttl_seconds` |
| `wal-retention-size` (default 1 GiB) | `wal_retention_size` | `wal_retention_bytes` | none: `WAL_size_limit_MB` is 0, `trim_wal` bounds the size |
| `wal-retention-hold` (default 8 GiB) | `wal_retention_hold` | `wal_hold_bytes` | none |

With a TTL set, RocksDB moves WAL files that recovery no longer needs into the `archive/` subdirectory instead of
deleting them, and deletes archived files older than the TTL. `RocksStorage::wal_retained()` is true when
`wal_retention_s > 0` and the write path is `wal` or `wal-buffered` (`no-wal` and `memory` write no WAL).

RocksDB would enforce a `WAL_size_limit_MB` only on a slow periodic check (every 10 minutes), which a heavy
write load can overshoot far, and without knowing what replicas still need. So the size bound is
`RocksStorage::trim_wal()`'s alone, and the `Maintenance` thread in [`main.cpp`](../../src/main.cpp) calls it
every second when the WAL is retained:

1. Return at once unless the WAL is retained and `wal_retention_bytes > 0`.
2. `GetSortedWalFiles`, sum the sizes of the `kArchivedLogFile` files.
3. Walk the files oldest first, deleting archived files (with `std::filesystem::remove` on
   `dir / PathName()`) until the archived total is within the limit. Live files are never touched. A failed
   delete logs a warning and stops.
4. A file that holds sequences from the held one on (`hold_wal(seq)`: the next file starts past `seq`) stops
   the walk, unless the archived total is past the limit plus `wal_hold_bytes`. The primary holds the oldest
   `needs` of its replicas: the snapshot's `next` while one does a full sync (a 10 GB sync under heavy writes
   outlasted 1 GB of WAL, so the replica synced again, and again), then its acknowledged offset.

A reader still positioned in a deleted file gets the "no longer in the kept WAL" error on its next read.

## RocksStorage WAL APIs

All in [`rocks_storage.h`](../../src/storage/rocks_storage.h) /
[`rocks_storage.cpp`](../../src/storage/rocks_storage.cpp):

| API | Behavior |
|---|---|
| `latest_sequence()` | `DB::GetLatestSequenceNumber()`: the sequence of the last write (0: none yet) |
| `oldest_sequence()` | `StartSequence()` of the first file from `GetSortedWalFiles` (live and archived); `latest_sequence() + 1` if there are no files or the call fails |
| `wal_bytes()` | total size of all WAL files, live and archived (`CDC.INFO wal_bytes`) |
| `wal_from(from)` | private; the checks and the seek below, as a `TransactionLogIterator` (null if nothing was written since) |
| `for_each_wal_batch(from, fn)` | private; the iteration both readers share (below) |
| `read_wal(from, WalVisitor&)` | decodes every batch from `from` and hands its entries to the visitor |
| `read_wal_raw(from, max_bytes, &out)` | whole undecoded batches as `RawBatch{seq, count, data}` until about `max_bytes` were collected (always at least one) |
| `WalReader(storage, from)`, `read(max_bytes, &out)` | the same, keeping its place between reads (below) |
| `cf_ids()` | this database's column family ids, as `CfIds` (`meta, data, index, system`) |
| `decode_batch(data, ids, visitor)` | decodes a batch written by a database whose column family ids are `ids` (no `begin`/`end` calls) |
| `on_next_write(after_seq, fn)` | registers `fn` to run once after the next write, or returns 0 (and never calls `fn`) if `latest_sequence() > after_seq` already |
| `cancel_write_wait(id)`, `cancel_all_write_waits()` | remove one or all registrations (the server cancels all on shutdown) |

`for_each_wal_batch(from, fn)`:

- throws `StorageError("the WAL is not kept (wal-retention 0)")` unless `wal_retained()`;
- throws `sequence N is no longer in the kept WAL (the oldest is M)` if `from != 0 && from < oldest_sequence()`;
- starts at `from`, or at `max(oldest, 1)` when `from == 0`, and returns at once if that is past
  `latest_sequence()`;
- iterates `DB::GetUpdatesSince(start)`. If the first batch returned starts after `from` (a gap: the batch
  holding `from` is gone), it throws the same "no longer in the kept WAL" error;
- calls `fn(seq, batch)` until it returns false. A final iterator status of `NotFound` or `TryAgain` is the
  normal end of the WAL; any other error throws.

Seeking is not cheap: `GetUpdatesSince` lists the WAL files (`GetSortedWalFiles`, which reads the start of
each) and reads the file holding `from` from its beginning. A replica's stream would pay that for every batch
(with 200 MB of WAL, `SET` + `WAIT 1` took 9 ms instead of 0.25), so `stream_wal` reads through a `WalReader`,
which keeps its iterator: at the end of the WAL a later `Next()` reads what was written since (RocksDB's log
reader re-checks the tail), and only when that says `TryAgain` (RocksDB went on to a new WAL file) or fails does
the reader seek again with `wal_from`, which also re-checks that the WAL is still kept.

`WalVisitor` is the decoding interface: `begin(seq, count)` (false stops before the batch), `put`, `del`,
`delete_range` (with the `Cf` already resolved), `log_data(blob)` and `end()`. `BatchDispatcher` adapts it to
`rocksdb::WriteBatch::Handler`; `SingleDelete` is reported as `del`, `Merge` is ignored (never written), and an
unknown column family id is reported as `Cf::System`.

The write wake-ups: every `put`, `del`, `write` and `delete_all` calls the private `wrote()` after the RocksDB
write. `wrote()` returns immediately unless `has_write_waits_` is set; otherwise it takes every registration
under `write_waits_mu_` and runs the callbacks on the writing thread. Callbacks must therefore be cheap; the only
one, in `wait_for_write`, posts a timer cancellation to the waiting connection's executor.

## Change notes

A data CF entry such as `u32 len | "k" | version | "abc" -> "1"` does not say whether `abc` is a hash field, a set
member or something else, and a batch does not carry its commit time. When the WAL is retained, every batch of
user data therefore carries a note written with `RocksStorage::Batch::put_log_data` (`WriteBatch::PutLogData`),
which goes to the WAL only:

```text
'C' (0x43) | u8 format = 1 | be64 time_ms
  per collection version whose elements the batch writes:
    be32 prefix length | data prefix (u32 key length | key | be64 version) | u8 ValueType
```

`change_note(time_ms)` builds the header, `add_change_note_type(note, prefix, type)` appends an entry (both in
[`change_feed.cpp`](../../src/db/change_feed.cpp)). The `Database` decides at construction
(`change_notes_ = storage.wal_retained()`) whether to write notes:

- `KeyTxn::note_type()` runs on every element `put`/`del` and records `data_prefix(key, version) -> type` once
  per collection version in `PendingBatch::element_types`. `Database::commit_batch` writes the note first in the
  batch.
- Writes outside a pending batch add a note of their own: `write_storage` and `delete_storage` (a single record
  write becomes a batch with a note), `mset`, `rename` (with the destination's type entry) and `flush_all`
  (passed to `RocksStorage::delete_all`).
- Not noted: `System` writes (version mark, index definitions, replication state), `drop_elements` and
  `write_index` when called without a pending batch, and the full-sync snapshot batches. None of them carry
  elements that need a type, but their batches have no commit time.

## Change feed decoding

`read_changes(storage, from, max_changes, max_batches, keep)` in [`change_feed.cpp`](../../src/db/change_feed.cpp)
reads `latest_sequence()` first (so nothing written during the read is skipped by the returned cursor), then runs
`read_wal` with a `Decoder`.

Per batch, `Decoder`:

1. `begin(seq, count)`: stops once `max_changes` changes were collected or `max_batches` batches looked at;
   otherwise resets the per-batch state and remembers `next_ = seq + count`.
2. `log_data`: parses a note (ignores blobs that are not `'C'` format 1): the time, and the prefix-to-type map.
3. `put` / `del` / `delete_range` as they come:
   - meta put: decoded right away into `set`, `json` or `meta` (below);
   - meta delete: `del`;
   - meta `DeleteRange`: marks the batch as a `FLUSHALL` (the only range delete of records);
   - data put / delete: queued as an element, with its data prefix;
   - data `DeleteRange`: remembers `begin` as a dropped collection version;
   - `index` and `System` entries are ignored (index entries are derived data).
4. `end()`: for a FLUSHALL the batch becomes the single change `flushall` and queued elements are dropped.
   Otherwise each queued element whose prefix is not a dropped version is decoded with the type from the note.
   Elements are decoded last because the note may appear anywhere in a batch. Batches left without changes are
   not returned, but still advance the cursor.

Derived operations (see the table in [`change_feed.h`](../../src/db/change_feed.h)):

| Source | Change | Args |
|---|---|---|
| meta put, `String` | `set` | value, expire_at_ms |
| meta put, `Json` | `json` | document text, expire_at_ms |
| meta put, collection | `meta` | type name, size, expire_at_ms, version, and for lists the head as a position (`head - kListStart`) |
| meta delete | `del` | |
| meta `DeleteRange` | `flushall` (key `""`) | |
| data, Hash | `hset` / `hdel` | field [value] |
| data, Set | `sadd` / `srem` | member |
| data, ZSet `'m'` entry | `zadd` (score from the 8-byte value) / `zrem` | [score] member; `'s'` entries are skipped |
| data, List | `lset` / `ldel` | position (`be64 sub-key - kListStart`) [value] |
| data, no type in the note | `data` / `datadel` | version, sub-key [value] |

Notes on ordering and filtering:

- Within a batch, record changes come in batch order and element changes after them. Batches built by
  `commit_batch` write meta entries from an `unordered_map`, so record changes of one pipeline are not in command
  order.
- `keep(key)` filters every change except `flushall`. `CDC.READ` passes `MATCH` (glob) combined with the ACL check
  `may_read_key`.
- `ChangeRead::next` is `next_` of the last batch looked at; if no batch was looked at, `from` (or
  `latest + 1` when `from` was 0).
- Gaps are detected by `for_each_wal_batch` and surface as `StorageError`, which `CDC.READ` returns as
  `ERR sequence N is no longer in the kept WAL (the oldest is M)`.
- Expirations done by the meta compaction filter never reach the WAL; that is why `set` and `meta` carry
  `expire_at_ms`.

## CDC.READ and BLOCK

`cmd_cdc_read` in [`cmd_cdc.cpp`](../../src/server/cmd_cdc.cpp) parses `<sequence|$> [COUNT n] [MATCH pattern]
[BLOCK ms]`. `$` means `latest_sequence() + 1`; `COUNT` defaults to `kDefaultCount` = 100 and is reached with
whole batches; one read looks at no more than `kMaxBatchesPerRead` = 10000 batches.

Blocking reuses the connection's "retry later" mechanism in `Server::serve_redis`
([`server.cpp`](../../src/server/server.cpp)) and never holds an I/O thread:

```text
client            serve_redis (strand)            cmd_cdc_read                RocksStorage
  │ CDC.READ $ BLOCK 0 │                               │                            │
  │───────────────────▶│ execute_redis_command ───────▶│ from = latest + 1          │
  │                    │                               │ read_changes: nothing      │
  │                    │                               │ block_deadline_ms = max    │
  │                    │                               │ block_from = next          │
  │                    │                               │ wait_after_seq = next - 1  │
  │                    │◀── retry_later, wait_for_writes                            │
  │                    │ commit batch, flush earlier replies                        │
  │                    │ wait_for_write(after_seq, deadline) ── on_next_write ─────▶│
  │                    │      (coroutine suspended on a steady_timer)               │
  │                    │                                    some client writes ────▶│ wrote(): run callback
  │                    │◀──────────────── asio::post(ex, timer->cancel()) ──────────│
  │                    │ cancel_write_wait(id), rerun the same command              │
  │                    │ execute_redis_command ───────▶│ resuming: from = block_from│
  │                    │                               │ read_changes: 1 batch      │
  │◀───────────────────│◀──── [next, [[seq, time, changes]]]                        │
```

- `client.block_deadline_ms != 0` marks a rerun; `$` was resolved on the first attempt only, through
  `block_from`.
- `BLOCK 0` uses `INT64_MAX` as the deadline (`wait_for_write` then waits on `time_point::max()`); otherwise the
  deadline is `steady_time_ms() + ms`. When the deadline passes, the timer fires, the command reruns and replies
  with the empty result.
- Blocking requires `client.can_block`, which `serve_redis` sets; elsewhere `BLOCK` returns at once.
- `wait_for_write` in [`asio_util.h`](../../src/server/asio_util.h) returns immediately if `on_next_write` says the
  write already happened (id 0). The callback is posted to the connection's executor, so it cannot run before the
  wait starts.

## Replication protocol

Replication runs over the Redis protocol on the normal port. Message format: commands and stream messages are
RESP arrays of bulk strings; the handshake replies are simple strings.

### Handshake

The replica (`connect_and_sync` in [`replica_link.cpp`](../../src/server/replica_link.cpp)) sends, waiting for a
one-line reply to each:

| Request | Expected reply |
|---|---|
| `AUTH [masteruser] masterauth` (only when `masterauth` is set) | `+OK` |
| `PING` | `+PONG` |
| `REPLCONF listening-port <port>` | `+OK` |
| (on the stream, from the replica) `REPLCONF ACK <offset>`, `REPLCONF FAILOVER-ACK <offset>`, `REPLCONF HANDOFF <offset>` | none |
| `REPLSYNC <replid> <offset>` | `+FULLSYNC ...` or `+CONTINUE ...` |

`REPLSYNC` sends the primary id and offset stored in `repl:primary-id` / `repl:offset` when both exist and no full
sync is in progress (`!ReplicationState::syncing()`). A server with no `repl:primary-id` but its own `repl:id`
(a primary until now) sends its own id and `latest_sequence() + 1`: its own history, which a replica it handed
over to can continue. Else `? 0`. On the primary, `cmd_replsync`
([`cmd_repl.cpp`](../../src/server/cmd_repl.cpp)) refuses with
`ERR replication needs the WAL kept on the primary: set wal-retention` unless `wal_retained()`, then stores the
id and offset in the `ClientState` and sets `start_replication`. `serve_redis` stops parsing, flushes the
replies so far and hands the connection, with any bytes already read, to `serve_replica`.

### Replies

```text
+CONTINUE <replid> <next> <meta id> <data id> <index id> <system id>
+FULLSYNC <replid> <next> <meta id> <data id> <index id> <system id>
```

`replid` is the primary's 40-character id (`ReplicationState::own_id`), `next` the sequence the WAL stream
starts at, and the four numbers the primary's `cf_ids()`, which the replica needs to decode raw batches. The
replica rejects any reply that is not 7 words long.

### Stream messages (primary to replica)

| Message | Meaning |
|---|---|
| `["snap", batch]` | full sync only: a `rocksdb::WriteBatch` of puts copied from the snapshot (about `kReplBatchBytes` = 1 MiB) |
| `["snapend"]` | full sync only: the snapshot is complete |
| `["wal", next, batch]` | one WAL batch, as RocksDB stored it; `next` = `seq + count`, the sequence after it |
| `["ping"]` | sent when nothing was sent for `kPingIntervalMs` = 1000 ms |

Batches are RocksDB's own `WriteBatch` representation (`WriteBatch::Data()`), including log data such as change
notes, with the primary's column family ids inside.

### Acknowledgements (replica to primary)

`REPLCONF ACK <offset>`, where `offset` is the replica's next sequence to fetch. `send_acks` is the only
writer of the replica's connection once the stream runs: it sends an ACK as soon as `read_stream` applied
something (`read_stream` cancels `Session::ack_timer` after each round of messages), and otherwise every
`kAckInterval` = 1 s. It records the offset it is sending before the write, so an offset that moves on during
the write is acknowledged on the next turn. The primary's `read_acks` parses them on the stream connection and
calls `ReplicationState::replica_ack` (offset and time, shown by `INFO replication`), which also fires the
waiters of `WAIT` (`wait_ack`: one-shot `KeyWatch::Waiter`s; the waiting connection counts `acked(offset)`
again when woken, and `Server::wait_for_keys` forgets the waiter when the wait ends). As an ordinary command,
`REPLCONF ACK` is accepted and never answered.

### TLS

With `tls-replication`, `connect_and_sync` wraps the connected `tcp::socket` in an
`asio::ssl::stream<tcp::socket&>` built from `replication_client_tls` ([tls.cpp](../../src/server/tls.cpp)):
`verify_peer` against `tls-ca-cert-file` (or the default paths), SNI and `host_name_verification` for the
`replicaof` host, and the client certificate from `tls-client-cert-file` or `tls-cert-file`. The rest of the
replica side (`sync_with`, `read_stream`, `send_acks`, `Session<Stream>`) is templated over the stream, and
`ReplicaLink::Shared::socket` still points at the TCP socket, so `stop()` closes either.

### Failover

`FAILOVER` runs in `Server::run_failover`, a coroutine on the server's `io_context`:

```text
client ──FAILOVER──▶ primary: writes_paused = true (writes get READONLY); "waiting-for-sync"
                     poll every 10 ms: target ack_offset >= latest_sequence() + 1  (or TIMEOUT, FORCE)
                     request_failover(replica, at, force); wake_write_waiters(); "failover-in-progress"
primary stream_wal ── failover <at> [force] ─────────────────▶ replica read_stream: offset == at (or force)
                                                               send_acks:
primary read_acks ◀── REPLCONF ACK <at>, REPLCONF FAILOVER-ACK <at>
  failover_ack: failover_taken_by_ = replica                   stopped = true; post ctx.replicaof("", 0)
run_failover sees it: follow(ip, listening port); writes_paused = false; "no-failover"
```

The pause is a flag of its own (`ServerContext::writes_paused`, checked in `execute_redis_command`), not
`Database::set_read_only`: a batch that commits while the database is read-only is discarded (`commit_batch`
drops a replica's own writes), which would lose writes already under way. `take_failover` sends the message only
when the stream has caught up (`raw.empty()`) and `next >= at`; a replica at another offset ignores it, the
primary times out after 5 s and takes writes again. The acknowledgment is kept in `failover_taken_by_`, not in the
replica's entry, because the replica disconnects right after sending it.

### Promotion with a hand-over

With `promotion-handoff` the replica starts the same exchange (`Server::follow_locked`, `ReplicaLink::request_handoff`):

```text
client ──REPLICAOF NO ONE──▶ replica: link streaming, not syncing, promotion-handoff
                             handoff_pending_ = true; handoff_timer_ 5 s; reply OK (still a replica)
replica send_acks ── REPLCONF HANDOFF <offset> ──────────────▶ primary read_acks: ctx.handoff(replica)
                                                               Server::hand_over: start_failover(that replica,
                                                               1000 ms, no force)  (the FAILOVER steps above)
replica ◀── failover <at> ── ... FAILOVER-ACK ... post ctx.replicaof("", 0)
follow_locked("", 0): handoff_pending_ was set: becomes a primary
```

`request_handoff` sets `Shared::handoff` and cancels the acknowledging side's timer (`Shared::ack_timer`, on the
link's strand), so `send_acks` sends it at once; it returns false, and the replica promotes itself at once, when
the link is stopped or not streaming (`Shared::streaming`, set around the stream phase of `sync_with`). Every
`follow_locked` moves `follow_gen_` on and cancels the timer; the timer's handler promotes only if the generation
is still its own and the hand-over is still pending, so a `REPLICAOF host port` meanwhile wins. The primary's
catch-up limit (1 s) is shorter than the replica's wait (5 s): a primary that gives up takes writes again before
the replica promotes itself anyway, which leaves the window there was without the option, and no worse.

A server that becomes a replica closes its replicas' streams (`e->replica` connections in `ctx.clients`), and
`cmd_replsync` refuses on a replica. Without that, the old primary's other replica, still streaming from it,
received the old primary's own writes as a replica (the continuation batch in the system column family, then
everything it applied), whose sequence numbers are the old primary's, past the promotion point: re-pointed by
Sentinel a second later, it synced in full.

### Full sync

```text
replica (ReplicaLink)                                   primary (serve_replica)
  │ REPLSYNC ? 0  (or an id/offset that cannot continue)    │
  │────────────────────────────────────────────────────────▶│ snap = snapshot(); next = snap.sequence + 1
  │◀──────────────── +FULLSYNC id next ids ─────────────────│
  │ set_link("sync"), set_syncing(true) (LOADING)           │ for cf in meta, data, index, system:
  │ clear_for_full_sync()                                   │   cursor over snap (skip "repl:" keys)
  │◀──────────────── snap <batch> ──────────────────────────│   batch.put(...), flush at ~1 MiB
  │ apply_replicated(batch, ids, {})                        │
  │◀──────────────── snap <batch> ... ──────────────────────│
  │◀──────────────── snapend ───────────────────────────────│ replica_state("online")
  │ finish_full_sync(next): repl:primary-id, repl:offset,   │
  │   repl:id in one write; set_syncing(false)              │
  │◀──────────────── wal <next'> <batch> ───────────────────│ stream_wal(next)
  │ apply_replicated(batch, ids, {repl:offset = next'})     │
  │ ── REPLCONF ACK <offset> (every second) ───────────────▶│ read_acks
```

### Partial sync (continue)

```text
replica                                                 primary
  │ REPLSYNC <primary id> <offset>                          │
  │────────────────────────────────────────────────────────▶│ id matches, 0 < offset,
  │                                                         │ oldest_sequence <= offset <= latest + 1
  │◀──────────────── +CONTINUE id offset ids ───────────────│
  │ set_link("connected")                                   │ stream_wal(offset)
  │◀──────────────── wal / ping ... ────────────────────────│
```

## Primary side: serve_replica

`serve_replica<Stream>(ctx, stream, client, in)` is instantiated for plain TCP sockets and for
`asio::ssl::stream<tcp::socket>`, so a replica can also be served on the TLS port.

1. Decide: partial if `client.repl_id == own_id`, `offset > 0`, `offset >= oldest_sequence()` and
   `offset <= latest_sequence() + 1`; else partial if `continue_previous` maps the offset into this server's own
   WAL (below, [Promotion](#promotion)); otherwise full. `replica_needs` holds the WAL from `next`.
2. `ReplicationState::add_replica(ip, listening_port, full)` (state `sync` or `online`); a guard removes it when
   the coroutine ends.
3. Full sync: take a RocksDB snapshot, reply `+FULLSYNC` with `next = snapshot->sequence() + 1`, then iterate
   `meta`, `data`, `index` and `System` at that snapshot (`fill_cache = false`), skipping `System` keys that start
   with `repl:`. Puts accumulate in a `RocksStorage::Batch`, sent as `snap` whenever it reaches `kReplBatchBytes`
   and at the end of each column family; `snapend` closes the snapshot.
4. Mark the replica `online` and run `first_to_end(stream_wal(next), read_acks(...))`: whichever ends first,
   returning or throwing, cancels the other, and its exception is rethrown. Not `operator||`, which waits for
   the first to end *without* an exception: a `stream_wal` that threw would leave `read_acks` reading the
   replica's acknowledgments for good, and the replica would never sync again.

`stream_wal` loops:

- `WalReader::read(kReplBatchBytes, &raw)`. It throws when the replica fell behind the kept WAL; the
  exception ends `serve_replica` (logged), the connection closes, and the replica's next `REPLSYNC` gets a full
  sync because its offset is now older than `oldest_sequence()`.
- With batches: append one `wal` message per batch, advance `next`, flush.
- Without: send `ping` if nothing was sent for a second, then `wait_for_write(next - 1, now + 1000 ms)`.

## Replica side: ReplicaLink

`ReplicaLink` ([`replica_link.h`](../../src/server/replica_link.h)) is created by `Server::follow(host, port)`
(startup with `replicaof`, or `REPLICAOF host port`) and runs one coroutine on its own strand. `Shared` holds
what `stop()` (callable from any thread) needs: the `stopped` flag and pointers to the current socket and pause
timer, which `stop()` closes and cancels on the strand.

```text
           ┌────────────────────────────────────────────────────┐
           ▼                                                    │
   set_link("connecting") ─▶ connect (5 s timeout) ─▶ handshake │
           │                                                    │
           ├─ +FULLSYNC ─▶ set_link("sync"), syncing, clear ─┐  │
           ├─ +CONTINUE ─▶ set_link("connected") ────────────┤  │
           │                                                 ▼  │
           │              first_to_end(read_stream, send_acks)  │
           │                        (snapend ─▶ "connected")    │
           │                                                    │
   any error, close, or 10 s without data ─▶ set_link("down") ─▶ wait 1 s
```

| Constant | Value | Use |
|---|---|---|
| `kConnectTimeout` | 5 s | `async_connect` deadline |
| `kPrimaryTimeout` | 10 s | `read_stream` gives up when nothing (not even a ping) arrived for this long |
| `kReconnectDelay` | 1 s | pause before the next attempt |
| `kAckInterval` | 1 s | `REPLCONF ACK` period |

`run()` loops `connect_and_sync` until stopped; every exception is logged as a warning and followed by the
reconnect delay. A stopped link leaves the link state alone: `REPLICAOF` (what Sentinel sends) stops it and
starts the next one at once, and its late `down` would otherwise land after the new link's `connected`. `read_stream` parses messages with `RespParser` and hands each to `handle()`:

- `wal next batch`: `db.apply_replicated(batch, ids, {{"repl:offset", be64(next)}})`, then `offset = next`
  and `ReplicationState::set_offset`.
- `snap batch`: `db.apply_replicated(batch, ids, {})`.
- `snapend`: `finish_full_sync(next)` writes `repl:primary-id`, `repl:offset`, `repl:segment` (`next` and this
  server's next sequence: where its copy of the history began) and `repl:id` (the clear removed it) in one
  batch, clears `syncing`, counts the full sync and sets the link `connected`.

A `+CONTINUE` whose id is not the one sent (the primary continues this server's history after a promotion)
first writes the new `repl:primary-id`, `repl:offset` and `repl:segment`, and drops `repl:previous`.
- `ping`: nothing (any message resets the 10-second timer and `touched_primary()`).
- anything else: an error, which drops the link.

Because the offset is written in the same RocksDB write as the batch it follows, a crash can never leave the
replica with data and offset out of step: after a restart it continues exactly after the last applied batch.

## Applying batches on the replica

`Database::apply_replicated(data, ids, system_puts)` in [`database.cpp`](../../src/db/database.cpp):

1. `RocksStorage::decode_batch(data, ids, ReplicaBatch)` rebuilds the primary's batch with this database's
   column family handles. `ReplicaBatch` (a `WalVisitor`):
   - skips puts and deletes of `System` keys that start with `repl:` (the primary's own state);
   - copies everything else, including `DeleteRange` and log data (so the primary's change notes reach this
     replica's WAL too);
   - collects the user keys touched: meta keys, the user key of data keys (`decode_data_key`), and the key of a
     data `DeleteRange` (whose bounds are data prefixes); a meta `DeleteRange` (FLUSHALL) sets `everything`;
   - sets `index_defs` when a `System` key starting with `index:` changes.
2. `system_puts` (the replica's `repl:offset`) go into the same batch.
3. Under the key locks (`KeyLocks::lock_keys(keys)`, or `lock_all()` for `everything`), write the batch, then
   clear the cache (`everything`) or erase each touched key from it. Readers that miss the cache load under the
   same stripe lock, so none can put a value older than this batch back into the cache.
4. If `index_defs`, `IndexRegistry::reload()` after the write: definitions created, dropped or changed (state,
   error) on the primary take effect immediately.

The replica writes the batch with its own sequence numbers; `repl:offset` and `INFO replication` speak in the
primary's sequence numbers.

`Database::clear_for_full_sync()` runs before the snapshot is applied: under `lock_all()`, `delete_all()` empties
`meta`, `data` and `index`, a second batch deletes every `System` key (including the version mark, index
definitions and the `repl:` keys), then the cache is cleared and the index registry reloaded (now empty).

The snapshot sends the `index` column family before the `System` one, so index entries arrive before the
definitions that explain them. A compaction in that window would see unknown index ids, which
`IndexRegistry::owner_of` reports as dropped (-1). So `clear_for_full_sync` also pauses the index GC
(`IndexRegistry::pause_gc`): while paused, `owner_of` keeps every entry. `Database::finish_full_sync` (called by
the link's `finish_full_sync` at `snapend`) reloads the definitions and resumes it, and so does `promote`, for a
sync that never finished. A replica restarted in the middle of a sync starts unpaused, but its next sync clears
everything again.

Index definitions arrive with the snapshot's `System` column family (sent last) and through later `wal` batches;
replicas never run backfills (`Database` is constructed with `resume_backfills = false`) because the primary's
backfill writes arrive as ordinary batches.

## Read-only replicas

- `main.cpp` and `Server::follow` call `Database::set_read_only(true)` on a replica.
- `execute_redis_command` ([`redis_commands.cpp`](../../src/server/redis_commands.cpp)) refuses commands in the
  write category with `READONLY You can't write against a read only replica.`, and, while
  `ReplicationState::syncing()`, commands in the read or write category with
  `LOADING datadam is loading the dataset from its primary`. The memcache side answers
  `SERVER_ERROR read only replica` to write commands (`MemcacheSession::permission_error`).
- Writes that reads make on their own (the lazy removal of an expired key, an index repair) are dropped inside
  the `Database`: `commit_batch` discards a read-only batch's staged writes, and `write_storage`,
  `delete_storage` and `drop_elements` return early. Expired keys still read as missing; their removal arrives
  from the primary.

## Promotion

`REPLICAOF NO ONE` calls `Server::follow("", 0)`:

1. Stop and destroy the `ReplicaLink`; return if the server is not read-only (already a primary).
2. `ReplicationState::set_primary("", 0)`, `set_syncing(false)`.
3. `keep_history_at_promotion` (still read-only, so no write of its own comes first): read `repl:primary-id`,
   `repl:offset` and `repl:segment` at one snapshot (the stopped link may still apply a batch), delete them, and
   write `repl:previous` = the old primary's id, the offset reached, `snapshot sequence + 1` (where the old
   history ends in this server's WAL) and the segment. Then a new replication id (`new_own_id`).
4. `Database::promote()`: read the `collection-version` mark (replicated from the old primary) and raise
   `version_` and `version_limit_` to it, so new collections get versions the old primary never used; reload the
   index registry; `set_read_only(false)`; start the backfill of every index still `Building`.

`continue_previous(storage, id, offset)` serves the old primary's other replicas, and the old primary itself:

- `id` must be `repl:previous`'s, and `segment.offset <= offset <= previous.offset`;
- at `previous.offset` exactly, the answer is `previous.own`;
- otherwise every batch this server applied from that primary is in its own WAL with `repl:offset` (the old
  primary's offset after it): `read_wal` from a little before (`4 * distance + 1024` sequences, never before
  the segment start or the oldest kept) with a `FindOffset` visitor finds the batch whose `repl:offset` is
  `offset`, and the replica continues after it. Not found (trimmed, or a gap): full sync.

A server past the promotion point (the old primary after taking writes, or a replica that acknowledged them)
gets `nullopt` and syncs in full: its extra writes are not in the new history. Only one previous history is
kept.

`REPLICAOF host port` on a primary or replica sets read-only, records the primary and starts a new link.

## Known limitations

- **Automatic failover is Redis Sentinel's.** datadam gives Sentinel what it reads (`INFO` fields, `run_id`,
  `REPLICAOF` and `CLIENT KILL` in `MULTI`, Pub/Sub); `tests/docker_sentinel_test.sh` checks a real failover.
- **Only the last history continues.** A promoted server keeps one `repl:previous`; replicas of an earlier
  primary sync in full. A replica of an unrelated primary always does.
- **Retention must cover a full sync.** The snapshot is sent before the stream starts at `next`; the WAL from
  `next` on is held, but only up to `wal-retention-hold` past `wal-retention-size`; past that, `stream_wal`
  throws and the replica syncs again.
- **`wal-buffered` delays readers.** With `manual_wal_flush`, a batch reaches the WAL file only when the
  maintenance thread flushes it (about every second), but `latest_sequence()` advances at once. Until then
  `read_wal` cannot return the batch while `on_next_write` already reports the write as done, so a blocked
  `CDC.READ` and `stream_wal` may retry repeatedly instead of sleeping until the flush.
- **Wake-up window.** `on_next_write` compares `latest_sequence()` under `write_waits_mu_`, but `wrote()` reads
  `has_write_waits_` without the mutex after the RocksDB write. A write that lands between the waiter's check and
  its registration can go unnoticed; waiters with a deadline (replication: 1 s) recover at the deadline, while
  `CDC.READ ... BLOCK 0` waits for the following write.
- **LOADING only during a live sync.** `syncing` is in memory. A replica restarted in the middle of a full sync
  (its `repl:` keys were cleared, so it will sync again) serves its partial data without `LOADING` until the
  primary answers `+FULLSYNC`.
- **CDC on a replica.** A replica that retains its own WAL has a change feed with its own sequence numbers. The
  primary's notes come through, but `clear_for_full_sync` reads as a `flushall` and the snapshot batches carry
  no notes, so their elements decode as `data` changes and their batches have time 0.

## Tests

[`tests/replication_test.cpp`](../../tests/replication_test.cpp) runs two complete servers in one process. Each
`Node` opens its own `RocksStorage` in a temporary directory (`fsync no`, `wal_retention_s` given per test), a
4 MiB `ObjectCache`, a `Database` (read-only, without backfills, when it has a `replicaof`), a `ServerContext`
and a `Server` started on ephemeral ports with `io_threads = 2`, running on its own thread. A blocking RESP2
`Client` talks to them, `eventually()` polls a condition for up to 15 s, and `caught_up()` compares the
replica's `slave_repl_offset` with the primary's `repl_offset` while the link is `up`.

| Test | Covers |
|---|---|
| `FullSyncThenStream` | full sync of every type and an index, streamed writes, cache invalidation, `READONLY`, `ROLE`, `IDX.DROP` and `FLUSHALL` following |
| `RestartedReplicaContinues` | a restarted replica continues with `+CONTINUE` (no second full sync) |
| `PromotedReplicaTakesWrites` | `REPLICAOF NO ONE`, new collection versions past the old primary's, the old stream no longer applied |
| `AfterAPromotionTheOthersContinue` | a replica that fell behind, and the old primary, continue from the promoted one (`continue_previous`), and on after a restart |
| `AnOldPrimaryThatWroteOnSyncsInFull` | an old primary with a write past the promotion point syncs in full, and loses it |
| `PrimaryNeedsAKeptWal` | `REPLSYNC` is refused without `wal-retention` |
| `ReplicaBehindTheKeptWalSyncsAgain` | `trim_wal` removes the WAL a replica needs, which then does a full sync |

[`tests/cdc_test.cpp`](../../tests/cdc_test.cpp) covers decoding of every type (including `RENAME` and `MSET`),
cursors, `COUNT`, `MATCH`, `$`, ACL key filtering, `BLOCK`, `CDC.INFO` and errors, restarts, size-based trimming
and JSON documents.
