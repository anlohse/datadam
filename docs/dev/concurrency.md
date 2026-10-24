# Concurrency

How datadam keeps many I/O threads, read pool threads and background threads correct on one keyspace: the key
lock stripes, per-key transactions, pipeline write batches and when they commit early, the read pool for cache
misses, the rules for publishing to the object cache, deadlock avoidance, the latency metrics, and the
asynchronous `BLOCK` of `CDC.READ`. Most of the code is in [database.h](../../src/db/database.h) and
[database.cpp](../../src/db/database.cpp).

## Contents

- [The locks at a glance](#the-locks-at-a-glance)
- [Key lock stripes](#key-lock-stripes)
- [Reads and writes of one key](#reads-and-writes-of-one-key)
- [Write batches](#write-batches)
- [When a batch commits early](#when-a-batch-commits-early)
- [Transactions: transact and transact_many](#transactions-transact-and-transact_many)
- [Publishing to the cache](#publishing-to-the-cache)
- [The read pool](#the-read-pool)
- [Blocking reads: CDC.READ BLOCK](#blocking-reads-cdcread-block)
- [Watching keys: KeyWatch](#watching-keys-keywatch)
- [Blocking commands: BLPOP and friends](#blocking-commands-blpop-and-friends)
- [MULTI/EXEC blocks](#multiexec-blocks)
- [Lua scripts](#lua-scripts)
- [Deadlock avoidance](#deadlock-avoidance)
- [Metrics](#metrics)
- [Rules for new code](#rules-for-new-code)

## The locks at a glance

| Lock | Where | Protects | Held for |
|---|---|---|---|
| World lock (shared / exclusive) | `KeyLocks` (`readers_` slots, `writer_`, `writers_`) | The whole keyspace | Shared: while a thread holds any stripe. Exclusive: a Lua script, or an `EXEC` that touches keys it does not name |
| Key stripe (`std::mutex`, 4096 of them) | `KeyLocks` in [key_locks.h](../../src/db/key_locks.h), owned by `Database` | One or more keys: the RocksDB record and its cache entry | A write until its batch commits; a cache-miss read until the cache is filled |
| Script engine mutex | `ScriptEngine::mu_` ([scripting.h](../../src/server/scripting.h)) | The Lua state, the script cache, the function libraries | One script run, or one `SCRIPT` / `FUNCTION` command |
| UNIQUE index mutex | `IndexRegistry::Index::unique_mu` ([index_registry.h](../../src/index/index_registry.h)) | The check-then-write of a UNIQUE value | Until the writing batch commits |
| Cache shard mutex | `ObjectCache::Shard::mu` ([object_cache.h](../../src/cache/object_cache.h)) | A shard's hash table, LRUs, slab allocator, stats | One lookup, insert or erase |
| Arena mutex | `PageArena::mu_` | Free page lists | One page acquire or release (inside a shard lock) |
| Collection version mutex | `Database::version_mu_` | Persisting the next version mark | Rarely: every 2^20 versions (`kVersionBlock`) |
| Others | `ReadPool::mu_`, `RocksStorage::write_waits_mu_`, `ServerContext::config_mu_`, `Acl::mu_` (shared), `IndexRegistry::mu_` (shared), `AclLog::mu_`, `ScanCursors::mu_`, `Doorkeeper::rotate_mu_` | Their own small state | Short, never while waiting for another lock of this list |

The order, from outermost to innermost, is: the world lock, then the script engine mutex, then key stripes
(ascending stripe number when several are taken together), then UNIQUE index mutexes (ascending index id), then
cache shard, then arena. A cache hit takes only the shard mutex. (A script holds the world lock exclusively,
so it takes no stripe at all.)

## Key lock stripes

`KeyLocks` has `kStripes = 4096` mutexes, each in its own 64-byte-aligned `Stripe` to avoid false sharing. A key's
stripe is `hash_bytes(key) & (kStripes - 1)`. Two keys may share a stripe; that only costs some contention, never
correctness.

Taking a stripe goes through two steps in `Database`:

1. `spin_try_lock` ([spin.h](../../src/util/spin.h)): up to 2000 `try_lock` attempts separated by `cpu_relax()`
   (`_mm_pause` on x86, `yield` on ARM). Key locks are held for microseconds, less than a sleep and wake-up
   through the kernel costs.
2. If that fails, `Database::wait_lock` blocks in `std::mutex::lock` and records the wait in the
   `lock_waits_` or `read_lock_waits_` histogram. The spinning is not timed.

The stripes are not reentrant. A write batch remembers the stripes it holds in `PendingBatch::held_mask` (a
`std::bitset<4096>`) and `held`, so a second command of the same pipeline on the same stripe does not lock it
again. `KeyLocks::Multi` locks a set of stripes in ascending order (sorted, duplicates removed) and unlocks them in
reverse; `lock_keys` and `lock_all` build one.

### The world lock

Over the stripes sits a reader-writer lock on the whole keyspace. A thread takes it shared when it goes from
holding no stripe to holding one (`lock`, `try_lock` and `spin_try_lock` do it on that transition, tracked in a
thread-local `Holding` slot per `KeyLocks`), and releases it with its last stripe. So it costs one atomic
increment and decrement per batch of stripes, not per stripe. The count is spread over `kReaderSlots` = 64
counters (`ReaderSlot`, one per cache line); each thread keeps the one it was given on first use. With a single
counter, every I/O thread would write the same cache line for every batch, which on a machine with many
cores turns into a bottleneck of its own.

`lock_world()` takes it exclusively: it must be called holding no stripe (`ExecScope` commits the batch
first). It takes `writers_` (one owner at a time), sets `writer_`, and waits until every reader slot drops
to 0, one after the other (a reader that comes in after its slot was seen empty sees `writer_` and steps
back, so one pass is enough);
both waits spin first (`kWorldSpins`), since owners and shared holders keep the lock for microseconds and a
wake-up through the kernel would leave the keyspace idle longer than that. A script takes the world lock
before the engine mutex (`ScriptEngine::eval_script` loads and runs under one hold of `mu_`), so the scripts
queued behind it spin on `writers_` instead of sleeping on `mu_`: that ordering tripled `EVAL` throughput. New
shared holders wait while `writer_` is set (writer preference: a stream of commands cannot starve a script;
the platform `shared_mutex` of glibc prefers readers). The owner then holds no stripe at all: with
`world_owned()` true, `KeyLocks::lock` and friends return at once, and `Database::lock_key`, `lock_for_read`
and `hold_stripe` do nothing. `unlock_world()` clears `writer_`, wakes the waiters and releases `writers_`.

There is no deadlock: a thread waiting for the world lock (shared or exclusive) holds no stripe, and the
exclusive owner waits for nothing but the shared holders, who never wait for it while holding stripes (a
thread that already holds a stripe takes more without touching the world lock).

## Reads and writes of one key

- **Read hit.** `Database::read`, `get`, `exists` and `peek` first call `ObjectCache::lookup`, which runs the
  callback under the shard mutex, straight from pooled memory. No key lock is taken.
- **Read miss.** `lock_for_read(key)` takes the key's stripe, then `load_locked` reads RocksDB (or the thread's
  own pending write, see below) and inserts the record into the cache. The stripe is released after the cache
  insert. Because a writer holds the same stripe while it writes, the value put in the cache can never be older
  than one a concurrent writer already stored.
- **Write.** `lock_key(key, reads)` takes the stripe; `store_locked` / `erase_locked` stage the RocksDB write and
  update the cache. Outside a write batch the `KeyGuard` releases the stripe when the call returns (the write
  went to RocksDB already); inside one the stripe joins the batch and is released at commit.

`Database::update(key, f)` is the read-modify-write primitive for strings and JSON: it takes the stripe with
`reads = true`, loads the record, calls `f(std::optional<Record>&)` and applies the returned `Database::Op`
(`Keep`, `Put`, `PutKeepCas`, `Delete`). When any index exists it opens its own `WriteBatchScope`, so the record
and its index entries reach RocksDB together. `Database::put` is a blind write: it does not read the old value
unless an index covers the key.

## Write batches

A `Database::WriteBatchScope` makes every write of its thread go into one `PendingBatch` until it commits. The
batch is found through a `thread_local` pointer (`tl_batch` in [database.cpp](../../src/db/database.cpp)), so:

- a scope must never span a coroutine suspension (the next step of the connection may run on another thread);
- a scope inside another scope of the same thread does nothing, and the outer one commits;
- `WriteBatchScope(db, false)` does nothing either: that is how `write-batching no` is implemented.

`Server::serve_redis` and `Server::memcache_session` open one scope per round over the input buffer (see
[architecture.md](architecture.md#the-life-of-a-request)); `transact`, `update` (with indexes) and `put` (with
indexes) open a local one that is a no-op inside the connection's.

What a `PendingBatch` holds:

| Member | Content |
|---|---|
| `writes` | Last write per `meta` key: the encoded record, or `nullopt` for a delete (hash map) |
| `data`, `index` | Last write per `data` / `index` key, in ordered maps so collection scans can merge them with RocksDB |
| `range_deletes` | Range deletes of dead collection versions, applied after the writes |
| `held`, `held_mask`, `held_since` | Stripes held and since when |
| `elements` | Decoded elements of small collections touched by the batch, published to the cache at commit |
| `element_types` | With the change feed on: the type of each collection version whose elements the batch writes |
| `batch_count`, `bytes` | Writes staged (before coalescing) and their approximate size |
| `in_txn`, `undo`, `range_mark` | The undo log of the transaction in progress |

The thread's own reads see its pending writes: `load_locked` and `exists_locked` check `pending(key)` before
RocksDB, `KeyTxn::get` checks `batch().data`, and `KeyTxn::scan` merges the ordered `data` map with a RocksDB
cursor (pending entries win; a pending delete hides the stored element).

`Database::commit_batch`:

1. On a read-only replica, drops whatever was staged (only reads ran; an expired key's removal is the primary's
   job).
2. Builds one `RocksStorage::Batch`: the change note first when the WAL is retained (`change_note` plus
   `add_change_note_type` for each `element_types` entry, see
   [replication-and-cdc-internals.md](replication-and-cdc-internals.md)), then `meta` puts/deletes, `data`,
   `index`, then the range deletes. Only the last write of each key is in it: 16 `INCR`s of one counter in a
   pipeline become one put.
3. `RocksStorage::write` (one `DB::Write`), then the `write_batches` / `write_batch_ops` counters.
4. `publish_elements` (see [Publishing to the cache](#publishing-to-the-cache)), still under the stripes.
5. `release_locks`: records how long the stripes were held in `batch_hold_`, unlocks in reverse order.

If the RocksDB write throws, the batch erases from the cache every key and element set it staged, while the
stripes are still held, marks itself `failed`, clears everything, releases the stripes and rethrows.
`WriteBatchScope::commit` turns a failed batch into `StorageError("write batch failed; replies discarded")`; the
session's `catch` then closes the connection without sending the replies of that round. The scope's destructor
commits too, but only logs a failure (`write batch dropped: ...`).

## When a batch commits early

A batch normally commits at the end of the round. It commits earlier in these cases, all of which keep it from
waiting (or reading the disk) while holding stripes:

| Where | Condition |
|---|---|
| `lock_key(key, reads = true)` | The batch already holds stripes, is not inside a transaction, and `key` is not cached: the command will read the disk, so the batch commits before locking `key` |
| `lock_key` | The batch reached `kMaxBatchWrites` (1024 staged writes) or `kMaxBatchBytes` (4 MiB) |
| `lock_key` | The stripe is busy (spinning failed) and the batch holds stripes: it commits, counts a conflict (`write_batch_conflicts`), then waits |
| `lock_for_read` | Busy stripe while holding others: commit, then wait. Free stripe while holding others: commit right after taking it, so the batch's keys are free during the disk read. (Inside a transaction it calls `lock_key` instead, joining the batch.) |
| `TxnRun` for `transact_many` | Always, before locking several stripes |
| `Database::mset`, `rename`, `flush_all` | Always (`commit_active_batch`): they lock their keys (or all stripes) with `KeyLocks::Multi` and write their own RocksDB batch |
| `Database::keys`, `scan_keys`, `approx_size` | Always: `KEYS`, `SCAN`, `DBSIZE` read RocksDB directly |
| `Database::query_index`, `search`, `search_keys` | Always: index queries read RocksDB, and the client's own writes must be visible |
| `cmd_save`, `cmd_bgsave` | Always, before `RocksStorage::flush` |
| `TxnRun::finish` | The transaction wrote to a UNIQUE index: commit while its `unique_mu` is held |
| `Database::update`, `Database::put` | A JSON document covered by a UNIQUE index was written: `commit_active_batch` while `unique_mu` is held |

The UNIQUE case is about visibility: the next writer of the same index must find the value in RocksDB, so the
batch that took it commits before releasing the index mutex.

## Transactions: transact and transact_many

`Database::transact(key, f)` runs `f(KeyTxn&)` with the key locked; `transact_many(keys, f)` runs `f(KeyTxns&)`
with several keys locked (repeated keys share one `KeyTxn`). Both open a local `WriteBatchScope` and use
`TxnRun`:

- **Locking.** One key: `lock_key(key, true)`, so the stripe is held by the batch until it commits. Several keys:
  commit the batch, then `hold_stripe` each stripe in ascending order.
- **Loading.** The `KeyTxn` constructor loads the record (`load_locked`). If an earlier transaction of the same
  batch left the decoded elements in `PendingBatch::elements`, they are taken from there (they are newer than
  the cache's). A collection of at most `kMaxCachedElements` (128) elements that is not cached whole is read
  entirely once, so this and the next commands work from memory (see [memory-and-cache.md](memory-and-cache.md)).
- **Staging.** `KeyTxn::put` / `del` stage `data` entries with an undo record (`PendingBatch::undo`); `create`,
  `reset` and `remove` stage range deletes of the old version; `meta()` and `size()` are edited in place.
  Before the first change, `capture()` remembers the key's index tuples.
- **Finishing.** `TxnRun::finish` takes the UNIQUE mutexes of the indexes involved (in id order), runs
  `prepare_indexes` on every `KeyTxn` (computes entry changes and throws `IndexConstraintError` on a UNIQUE
  clash, before anything is written), then `apply_indexes`, then `KeyTxn::finish`: an empty collection is
  deleted; changed elements are handed to `PendingBatch::elements`; a changed meta is stored with
  `store_locked`.
- **Rollback.** If `f` (or `prepare_indexes`) throws, `~TxnRun` replays the undo log backwards over the `data`
  and `index` maps, truncates `range_deletes` to `range_mark`, and marks the element sets of its keys as stale
  (`nullopt`), so the cache drops them at commit. Meta records are only written in `finish`, so they need no
  undo. The batch is left exactly as before the transaction and stays usable.

`Database::peek(key, need_elements, f)` is the read-only shortcut used by `read_collection`: if the cache has the
record (and, with `need_elements`, all its elements), `f` runs on a read-only `KeyTxn` built from a copy of the
cached record, with no key lock and no batch. Any write or uncached read in it throws `std::logic_error`. When the
cache cannot serve it, `read_collection` falls back to `with_collection`, a normal transaction.

## Publishing to the cache

Two kinds of cache updates happen during a batch, with different timing:

- **Records** (strings, JSON, collection meta). `store_locked` stages the encoded record and calls
  `cache_store`, which inserts it into the cache at once (or erases the key with `cache-on-write no`), marked
  **pending** when a batch holds it (`Item::kPendingBit`, the top bit of the item's `type` byte). A lookup
  without the key's lock (`Database::read`, `get`, `exists`, `peek`, the read pool's first check) treats a
  pending item as a miss and takes the locked path, which waits for the writer's stripe, so it only ever sees
  the value once it is in RocksDB. Lookups made with the stripe held (`load_locked`, `exists_locked`,
  `publish_elements`) pass `pending_ok`: whoever holds the stripe is the writer itself, so a pending item is
  its own write. After `storage_.write`, `commit_batch` calls `ObjectCache::confirm` on every written key; if
  the write fails, the keys are erased instead, before the stripes are released. A deferred read
  (`NonBlockingReads`) that meets a pending item waits for the lock on the I/O thread instead of going to the
  read pool: the writer is committing right then.
- **Elements of small collections.** `KeyTxn::finish` does not put elements in the cache: it hands them to
  `PendingBatch::elements`, and the next transaction of the batch on that key takes them from there.
  `publish_elements` runs inside `commit_batch`, after the RocksDB write and before the stripes are released.
  For each key it looks up the cached record, checks that it is still a collection with the same version (an
  `EXPIRE` may have changed the record, a `DEL` may have removed it), and inserts the record again with the
  element tail (`element_tail`: the `Complete` marker plus the `ElementSet` encoding, or `TooBig`). A `nullopt`
  entry (rolled back) erases the key. Lock-free readers (`peek`) therefore only ever see elements that are in
  RocksDB.

The rules that keep the cache from ever holding a value older than RocksDB:

1. Every cache fill after a miss happens under the key's stripe (`lock_for_read` + `load_locked`, or
   `load_into_cache`, which looks in the cache again after taking the lock).
2. Every write updates the cache under the same stripe, and a batch keeps it until the RocksDB write is done.
3. Multi-key writers (`mset`, `rename`) and `flush_all` hold all their stripes across the RocksDB write and the
   cache update.
4. A replicated batch (`Database::apply_replicated`) locks every key it touches (all stripes for a `FLUSHALL`),
   writes, and erases those keys from the cache.

## The read pool

With `read-threads > 0`, plain reads that miss the cache do not read the disk on an I/O thread.

**Deferral.** Before each command, `serve_redis` sets `ClientState::defer_blocked_reads`. `execute_redis_command`
runs a command inside a `Database::NonBlockingReads` scope when that flag is set and `rerunnable(spec)` says the
command only reads keys through `Database::read` / `get` / `exists` and has no other effect (`GET`, `MGET`,
`EXISTS`, `STRLEN`, `GETRANGE`, `SUBSTR`, `TYPE`, `TTL`, `PTTL`, `EXPIRETIME`, `PEXPIRETIME`, `JSON.GET`,
`JSON.MGET`, `JSON.TYPE`, `JSON.STRLEN`, `JSON.ARRLEN`, `JSON.ARRINDEX`, `JSON.OBJKEYS`, `JSON.OBJLEN`). Inside that
scope `lock_for_read` first asks `RocksStorage::probe` whether RocksDB can answer without a file read (a `Get`
with `read_tier = kBlockCacheTier`: memtables, the block cache, and filters there). If it can (`Found` or
`Missing`), it counts `memory_reads` and goes on to lock and read as usual: the read that follows is served
from the same memory. Only for `NeedsIo` does it, unless the batch already holds the key's stripe (then the
batch's own pending write is read), call `ObjectCache::defer_miss` (so the miss is not counted twice),
increment `deferred_reads` and throw `Database::WouldBlock{key}`. A key that is pending in the object cache
(another client is committing it) is never deferred either: the read waits for its stripe. `WouldBlock` deliberately does not derive from
`std::exception`, so generic handlers let it through.

**Retry.** `execute_redis_command` catches `WouldBlock`, rewinds the reply to the mark taken before the command,
sets `client.retry_later` and `client.load_key`, and un-counts the command. `serve_redis` then:

1. resets the parse position to the start of that request and ends the round (the batch commits, the replies of
   the earlier requests are flushed, in order);
2. prefetches: parses up to `kMaxPrefetchCommands` (256) of the following requests and, through
   `uncached_read_keys`, asks the pool to load (`ReadPool::load(key, nullptr)`) each key of a rerunnable command
   that is not cached and that the user may access;
3. waits with `async_load(*pool, client.load_key, ...)`: the pool's completion is posted to the connection's
   strand, so the I/O thread serves other connections meanwhile;
4. runs the same request again. If the key is still not cached (missing, expired, too large for the cache, or the
   load failed), `defer` is false for that one run and the command reads the disk on the I/O thread, which also
   reports any error.

The order of replies within a pipeline is preserved because nothing after the deferred request is executed
before it.

**ReadPool** ([read_pool.cpp](../../src/server/read_pool.cpp)):

- **Single flight.** `load` looks the key up in `flights_`; a key already queued or loading only gets another
  waiter (`joined` counter). Otherwise the key is queued with its first waiter.
- **Threads.** Each `run` loop takes `clamp(queue size / threads, 1, read-batch)` keys: a single key goes to
  `Database::load_into_cache`, several to `load_many_into_cache` (one RocksDB `MultiGet`). Batching only kicks in
  when the backlog exceeds the number of threads, since separate reads run in parallel and one MultiGet would
  serialize them.
- **load_into_cache.** Looks in the cache, takes the stripe with `lock_for_read` (a pool thread has no batch and
  no `NonBlockingReads` scope), looks again, reads the `meta` record, and inserts it with `admit = true`, bypassing
  the admission filter because a client is waiting for it. Expired keys are not inserted (the rerun expires them).
- **load_many_into_cache.** Takes stripes with `try_lock` only (so their order does not matter), reads the locked
  keys with one `MultiGet`, releases the stripes, then loads the busy keys one by one.
- **Shutdown.** `~ReadPool` finishes the loads under way and tells the waiters of queued ones "not cached".

`Database::load_into_cache` is also what makes the scheme safe: the cache fill follows the same stripe rule as any
other miss.

## Blocking reads: CDC.READ BLOCK

`CDC.READ ... BLOCK ms` waits for the next write without holding a thread ([cmd_cdc.cpp](../../src/server/cmd_cdc.cpp)):

1. `serve_redis` sets `ClientState::can_block` (connections from `execute_redis_command` in tests do not, and get
   an immediate empty reply).
2. When there is nothing to return, `cmd_cdc_read` sets `block_deadline_ms` (steady clock; `BLOCK 0` means
   `INT64_MAX`) on the first run, then `block_from` (where to resume), `wait_after_seq` (the last sequence seen),
   `wait_for_writes` and `retry_later`, and writes no reply.
3. `serve_redis` ends the round and calls `wait_for_write` ([asio_util.h](../../src/server/asio_util.h)): it arms
   a per-connection `steady_timer` at the deadline and registers `RocksStorage::on_next_write(after_seq, fn)`.
   If a write already took the sequence past `after_seq` it returns at once (id 0).
4. `RocksStorage::write` calls `wrote()` after every write; when waiters exist it runs their callbacks on the
   writing thread. The callback posts a `cancel()` of the timer to the connection's strand, so it cannot run
   before the wait starts.
5. After the timer fires or is cancelled, the wait is unregistered (`cancel_write_wait`) and the request runs
   again. Seeing `block_deadline_ms != 0`, `cmd_cdc_read` resumes from `block_from` (so `$` is resolved only once),
   and either replies, waits again, or replies empty once the deadline passed; it resets `block_deadline_ms`.

`~Server` calls `cancel_all_write_waits` so no callback touches a timer of a destroyed `io_context`.

## Watching keys: KeyWatch

`KeyWatch` ([key_watch.h](../../src/db/key_watch.h)), owned by `Database` (`key_watch()`), tells interested
parties about writes to particular keys. It holds, per key, two lists:

- `Watcher`s (WATCH): an atomic `dirty` flag that `touched(key)` sets.
- `Waiter`s (blocking commands): a one-shot `fire()` that runs a wake-up callback. `touched(key)` takes the
  key's waiters out of the registry and fires each; a waiter that wants to wait again registers again.

Every write path calls `touched(key)` with the key's stripe held: `Database::write_storage` and
`delete_storage` (every record write and delete, batched or not), `KeyTxn::put` and `KeyTxn::del` (collection
elements, whose meta record may not change), `mset`, `rename`, and `apply_replicated` on replicas.
`flush_all` and `clear_for_full_sync` call `touched_all()`. With nothing registered, `touched` is a single
atomic load of `entries_`.

Because writers call it under the stripe, a check made under the same stripe followed by registering cannot
miss a write: the writer either staged before (its batch holds the stripe until it commits, so the checker
waits and then sees the data) or touches after the registration. `Database::watch_key` therefore registers a
WATCH under `lock_key`, and blocking commands register from inside their transaction. `WatchedKeys` (one per
connection that used WATCH) unregisters its keys on `clear()` and in its destructor; it holds the registry by
`weak_ptr`, so a `ClientState` that outlives the `Database` (in tests) is harmless.

## Blocking commands: BLPOP and friends

The blocking list and sorted set commands ([cmd_list.cpp](../../src/server/cmd_list.cpp),
[cmd_zset.cpp](../../src/server/cmd_zset.cpp)) use the same rerun mechanism as `CDC.READ BLOCK`, with a
key-specific wake-up instead of "any write":

1. The command opens `transact_many` over its keys. If one has data it pops it and replies.
2. Otherwise, still inside the transaction (stripes held), it calls `block_for_keys`
   ([redis_commands.cpp](../../src/server/redis_commands.cpp)). Unless the connection cannot block
   (`can_block` false: inside `EXEC`, or in tests), this sets `block_deadline_ms` on the first try, registers
   one `KeyWatch::Waiter` on every key, stores it in `ClientState::key_waiter` with `wait_keys`, and sets
   `wait_for_keys` and `retry_later`. The command writes no reply.
3. `serve_requests` ([server.cpp](../../src/server/server.cpp)) flushes the earlier replies and awaits
   `Server::wait_for_keys`. It arms the connection's timer at the deadline, sets the waiter's wake-up (a post
   of `timer->cancel()` to the strand), and waits for the timer **or** for the socket to become readable
   (`lowest_layer().async_wait(wait_read)`, which works the same under TLS). Readable with nothing available
   means the client closed: the coroutine ends without rerunning the command, so it never pops an element it
   could not deliver. Readable with data means pipelined requests: they are read into `in` (up to
   `kMaxInputWhileBlocked`, after which the socket is no longer watched) and the wait goes on.
4. On wake-up or deadline the waiter is unregistered from every key and the command runs again from the same
   position of the input. It either takes data, blocks again with the same deadline, or replies nil once the
   deadline passed. `execute_redis_command` resets `block_deadline_ms` whenever a command finishes without
   `retry_later`.

Every waiter of a key is woken by each write to it; the ones that find nothing block again. There is no queue
of waiters, so the order in which they are served is not their arrival order.

## MULTI/EXEC blocks

[cmd_tx.cpp](../../src/server/cmd_tx.cpp) and `execute_redis_command` implement transactions. Queueing
happens in `execute_redis_command`: after the command lookup, arity and ACL checks (a failure sets
`ClientState::multi_error`), a command that `runs_in_multi` does not exempt is copied into
`ClientState::queued` and replied `QUEUED`.

`EXEC` collects the keys of every queued command with `for_each_key`; a command that has the `@read` or
`@write` category but names no key (`KEYS`, `FLUSHALL`, index queries...) makes it lock everything. Then
`Database::ExecScope` ([database.h](../../src/db/database.h)):

- uses the thread's write batch, or opens one (`WriteBatchScope`) when there is none;
- commits what the batch holds (so it holds no stripe), then takes every needed stripe in ascending order with
  `hold_stripe`, like `transact_many`, or, for a block that needs everything, the world lock exclusively
  (`all`; released after the final commit);
- sets `PendingBatch::exec`. While it is set, `commit_batch` writes what is pending but does **not** release
  the stripes, so the early commits of commands inside the block (see the table above) never let another
  client in. `mset`, `rename` and `flush_all`, which lock their keys themselves, skip their own locking when
  `exec_holds_locks()` (the block already holds them; taking them again would self-deadlock on the
  non-recursive stripe mutexes);
- `commit()` clears the flag, commits and releases everything.

With the stripes held, `EXEC` checks `WatchedKeys::dirty()` (a writer that staged before must have committed
for the block to get the stripe), unwatches, and runs each queued command through `execute_redis_command`
with `defer_blocked_reads` and `can_block` turned off. Inside the block, `lock_key` and `lock_for_read` find
the stripes in `held_mask` and do nothing, and reads see the batch's pending writes.

Inside a block, `commit_batch` does not confirm what it wrote: the keys go to `PendingBatch::exec_written`
(as do the records `mset` and `rename` cache and the elements `publish_elements` caches), and are confirmed
only when the block's final commit releases its stripes. So lock-free readers see none of the block until it
ends, even across early commits. The limit that remains: a block commits as one RocksDB batch only if none
of its commands commits early.

`Commands.ExecBlocksAreIsolatedFromEachOther` in [realtime_test.cpp](../../tests/realtime_test.cpp) checks
that blocks reading two keys never see another block's writes half done.

## Lua scripts

[scripting.cpp](../../src/server/scripting.cpp) and [cmd_script.cpp](../../src/server/cmd_script.cpp). One
`ScriptEngine` per `ServerContext` holds one LuaJIT state, used under `mu_`.

- **Atomicity.** `EVAL`, `EVALSHA` and `FCALL` (`with_keyspace`) open `Database::ExecScope(db, {}, true)`: the
  world lock exclusively, so the script runs alone, and its writes (made by `redis.call` through
  `execute_redis_command` with a script `ClientState`, in the scope's batch) commit together, confirmed in the
  cache only at the end, as for `EXEC`. Inside `EXEC` (`owns_all_keys()`), the script runs in the block, which
  took the world lock because a queued command has `@scripting`.
- **Committing without the whole keyspace.** Holding the world lock through the RocksDB write would make every
  script's commit serial. So the scope tracks keys (`ExecScope(..., all, track_keys)`): before each
  `redis.call`, `call_command` passes the command's keys (`for_each_key`) to `note_keys`, or calls
  `note_any_key` for a command that reads or writes keys it does not name (`KEYS`, `SCAN`, `FLUSHALL`, index
  queries...). At `ExecScope::commit`, if tracking survived, `KeyLocks::downgrade_world` locks those stripes,
  marks them `committing`, and gives the world lock back. The batch then commits like an `EXEC` on named keys,
  holding the stripes of every key the script read or wrote until its writes are in RocksDB and confirmed in
  the cache, so the script stays serializable: nothing it read can change before its writes are visible.
- **Committers do not hold the world.** A downgraded script is not counted in `readers_` (its `Holding` is
  `committing`; `committers_` counts them), so the next script takes the world lock at once and runs while the
  first one's write is still in RocksDB; their writes meet in RocksDB's group commit. The next owner must not
  use a key the committer still holds: `note_keys` calls `wait_committed(stripe)` (lock and unlock the stripe's
  mutex if it is marked) before the command runs, and `note_any_key`, or an `ExecScope` with `all` that does
  not track keys (`EXEC`), calls `wait_all_committed()`. Plain commands lock stripes as always, so they wait
  for a committer's stripes like for anyone's. A committer takes no stripe until it released its own.
  `LiveScripts.ScriptsAreAtomic` (read-modify-write of one counter from several connections) and
  `LiveScripts.NoReaderSeesHalfAScript` check it.
- **Lua errors and C++.** A Lua error unwinds the C stack (SEH on Windows), which C++ code built for C++
  exceptions only must not have live objects in. Every Lua C function that holds C++ objects is a non-inlined
  `impl` that leaves the error value on the Lua stack and returns -1; a thin wrapper (`raising<impl>`,
  `lua_call_cmd`) calls `lua_error`.
- **SCRIPT KILL.** A count hook (every 100,000 instructions; the JIT is off, since compiled code skips hooks)
  raises an error when `kill_` is set. `kill()` uses atomics only, so `SCRIPT KILL` runs on another I/O thread
  while the script holds `mu_` and the world lock; it refuses (`UNKILLABLE`) once `wrote_` is set.
- **Waiting for a script.** In `execute_redis_command`, a command for the keyspace (or a script, or `EXEC`)
  arriving while a script has run for `kScriptParkUs` = 1 ms gets a one-shot `KeyWatch::Waiter` from
  `wait_until_done()` and sets `retry_later` and `wait_for_script`; `serve_redis` waits on it (and on the
  socket) off the thread, then runs the command again. `run()` fires the waiters after clearing `running_`.
  Sooner than 1 ms, the command just waits for the world lock. After `kScriptBusyMs` = 5 s, commands reply
  `BUSY` (`allowed_while_script_busy` lists the exceptions).
- **Functions on replicas.** Libraries are stored in the System column family (`function:lib:<name>`) with a
  random `function:version`; `sync_libraries` reloads them when the stored version differs from the loaded
  one, so a replica follows the primary's `FUNCTION LOAD` without hooks in the replication code.

## Deadlock avoidance

- **A batch never waits for a stripe while holding others.** `lock_key`, `lock_for_read` and `hold_stripe`
  callers commit first when the stripe is busy (only `hold_stripe` in `transact_many` waits while holding, and
  only in ascending order after committing everything else).
- **The world lock is taken holding no stripe**, shared on a thread's first stripe and exclusively by
  `ExecScope` after it committed its batch; its owner takes no stripe until `downgrade_world`, which locks
  stripes nobody else can hold. The owner's waits for committers (`wait_committed`, `wait_all_committed`)
  are on threads that are only committing: they wait for nothing.
- **Several stripes are always taken in ascending order** (`KeyLocks::Multi`, `TxnRun` for `transact_many`),
  after the batch committed.
- **UNIQUE index mutexes are taken in index id order** (`TxnRun::finish`, `Database::lock_unique`), after the key
  stripes, and the batch commits before they are released. Nothing waits for a key stripe while holding one.
- **`load_many_into_cache` only uses `try_lock`**, and waits for busy keys after releasing the others.
- **The cache shard mutex is innermost.** The callback of `ObjectCache::lookup` runs under it and must not call
  back into the cache or the `Database` (`read_doc` in [cmd_json.cpp](../../src/server/cmd_json.cpp) copies the
  text and parses it outside, for example).
- **Write batches are thread-local and never span a `co_await`**, so a coroutine never suspends while holding a
  stripe.

`DatabaseTest.ConcurrentBatchesAreAtomicAndDeadlockFree`, `CollectionsTest.ConcurrentListPushesKeepEveryElement`
and `IndexTest.ConcurrentUniqueClaimsLetOneWin` exercise these rules with several threads.

## Metrics

All histograms are `LatencyHistogram` ([latency_histogram.h](../../src/util/latency_histogram.h)): lock-free
atomic buckets, four per power of two from 1 us to about a minute (108 buckets), so a percentile is reported as
its bucket's upper bound, within 25%. `INFO latency` prints each as `calls`, `p50`, `p99`, `p99.9`, `p99.99`,
`max`; `CONFIG RESETSTAT` resets them.

| `INFO latency` line | Source | Measures |
|---|---|---|
| `key_lock_wait_usec` | `Database::lock_waits_` | Blocking waits (after spinning) for a stripe to write |
| `key_read_lock_wait_usec` | `Database::read_lock_waits_` | Blocking waits for a stripe to load a missed key |
| `write_batch_hold_usec` | `Database::batch_hold_` | From a batch's first stripe to its release |
| `storage_get_usec`, `storage_multiget_usec`, `storage_write_usec` | `RocksStorage` | RocksDB `Get`, `MultiGet`, `Write` |
| `read_pool_load_usec` | `ReadPool::latency_` | From a load request to its completion |

Counters: `write_batches`, `write_batch_ops`, `write_batch_conflicts` in `INFO persistence`
(`Database::write_batch_stats`); `deferred_reads`, `read_pool_loads`, `read_pool_batches`, `read_pool_joined`,
`read_pool_queued` in `INFO stats`.

## Rules for new code

- Go through `Database` for every key access; do not call `RocksStorage` for user keys from a command.
- Never hold a `WriteBatchScope`, a `KeyGuard` or a `KeyTxn` across a `co_await`.
- Before reading RocksDB directly (a cursor over `meta` or `index`), call `commit_active_batch()` so the
  connection's own writes are visible and no stripes are held during the scan.
- Code that locks several keys must commit the active batch first and lock in ascending stripe order
  (`transact_many` does both).
- A `transact` callback may be rolled back: collect results in local variables and write the reply after
  `transact` returns (`with_collection` documents this).
- Do not call into the cache or `Database` from inside an `ObjectCache::lookup` / `Database::read` callback.
