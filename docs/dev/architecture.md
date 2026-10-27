# Architecture

This page is the big picture of the datadam server process: which threads exist, how a request travels from the
socket to RocksDB and back, how the main components depend on each other, how the process starts and stops, and
the design rules the rest of the code relies on. The other pages under `docs/dev/` zoom in on each part.

## Contents

- [Process layout](#process-layout)
- [Threads](#threads)
- [Components and dependencies](#components-and-dependencies)
- [The life of a request](#the-life-of-a-request)
- [Startup](#startup)
- [Shutdown](#shutdown)
- [Pub/Sub connections](#pubsub-connections)
- [Design principles](#design-principles)
- [Where to read next](#where-to-read-next)

## Process layout

datadam is a single executable, `datadam`, built from [main.cpp](../../src/main.cpp) and the static library
`datadam_core` (everything else under `src/`). One process holds:

- one RocksDB database (column families `meta`, `data`, `index` and the default one, used as `Cf::System`), wrapped
  by `RocksStorage` ([rocks_storage.h](../../src/storage/rocks_storage.h));
- one object cache, `ObjectCache` ([object_cache.h](../../src/cache/object_cache.h)), whose items live in a
  custom memory pool (`PageArena` + `SlabAllocator`, under [src/mem/](../../src/mem/));
- one keyspace object, `Database` ([database.h](../../src/db/database.h)), which owns the key locks, the write
  batching and the secondary index registry, and is the only thing commands talk to;
- one `ServerContext` ([redis_commands.h](../../src/server/redis_commands.h)) with what the protocol handlers
  share: settings, statistics, ACL users, the ACL log, scan cursors, replication state and the TLS context;
- one `Server` ([server.h](../../src/server/server.h)): an Asio `io_context` per I/O thread, the listeners (RESP, RESP over
  TLS, memcache, each optional), the read pool and, on a replica, the replication link.

There is no database number other than 0 (`SELECT` accepts only 0) and no clustering.

## Threads

| Thread(s) | Count | Created by | What runs there |
|---|---|---|---|
| I/O threads | `io-threads` (default 4) | `Server::run` | `asio::io_context::run` of an `io_context` of its own: the client connections handed to it (C++20 coroutines, each on its own strand), round-robin by `Server::accept_loop`, including the primary side of replication (`serve_replica`) on a replica's connection |
| Control (the main thread) | 1 | `main` calls `Server::run` | The control `io_context`, which serves no connection: the accept loops, the signal handler, the replica link coroutine and `FAILOVER`. A long command on an I/O thread cannot hold them up |
| Read pool | `read-threads` (default 64; 0 disables the pool) | `ReadPool` constructor ([read_pool.cpp](../../src/server/read_pool.cpp)) | `ReadPool::run`: loads keys that missed the cache for deferred plain reads (`Database::load_into_cache` / `load_many_into_cache`) |
| Maintenance | 1 | `Maintenance` in [main.cpp](../../src/main.cpp) | Cache idle/age/TTL eviction every `cache-maintenance-interval` (default 1000 ms); once a second, `RocksStorage::sync_wal` when `fsync everysec` or `experimental-write-path wal-buffered`; once a second, `RocksStorage::trim_wal` when the WAL is retained (`wal-retention > 0`) |
| Index backfills | one per index being built | `Database::start_backfill` ([index_ops.cpp](../../src/db/index_ops.cpp)) | `Database::run_backfill`: scans the `meta` column family under the index prefixes in chunks of 256 keys and writes their entries through `transact`; joined in `~Database` |
| RocksDB background jobs | `rocksdb-background-jobs` (default 4) | RocksDB | Flushes and compactions, including datadam's compaction filters (TTL, orphaned collection elements, stale index entries) |

The maintenance loop wakes up every `min(cache-maintenance-interval, 1 s)` and catches every `std::exception`,
logging it with `log_error`. The replica link is not a thread: `ReplicaLink::start` spawns a coroutine on its own
strand of the server's control `io_context` ([replica_link.cpp](../../src/server/replica_link.cpp)) that connects,
syncs, applies the stream and reconnects one second after the link drops.

Every connection runs on a strand of one I/O thread's `io_context` (`Server::accept_loop`), so the operations of one
connection never run concurrently, and they all run on that thread. Other connections of the thread run whenever
one suspends, which is why anything thread-local (the write batch, see [concurrency.md](concurrency.md)) must
never span a `co_await`. A context per thread rather than one shared by all: with one, every completion went
through a single scheduler lock and epoll, and 16 I/O threads did barely more than 8 (cloud phase 4, 2026-10-27).
The price: a connection waits for whatever runs on its thread. While a script runs (`EVAL`, `FCALL`), the
connections of its thread wait for its end, so `accept_loop` gives new connections to another thread: a client
connecting to send `SCRIPT KILL` gets through. The accept loop picks the thread once the connection is there and
creates the socket on that thread's `io_context`, since a socket completes its operations on its own context.
The cost: a command that holds its thread for long (a big `KEYS`, a slow script) delays the other connections of
that thread, not just one thread's worth of work.

## Components and dependencies

```text
main.cpp
 ├── Config                      config/config.{h,cpp}     file + --option parsing, validation, dump
 ├── RocksStorage                storage/rocks_storage.*   RocksDB: CFs, batches, cursors, snapshots, WAL,
 │                                                          compaction filters, write waits (on_next_write)
 ├── ObjectCache                 cache/object_cache.*      sharded cache; Doorkeeper (cache/admission.h)
 │     └── SizeClasses, PageArena, SlabAllocator            mem/
 ├── Database                    db/database.*             keyspace: key locks, write batches, KeyTxn,
 │     ├── RocksStorage, ObjectCache                        cache fill and publish, collections, indexes
 │     ├── KeyLocks              db/key_locks.h
 │     ├── ElementSet            db/element_set.*          small collections cached whole
 │     ├── IndexRegistry         index/index_registry.*    definitions (IndexDef), index_ops.cpp, search_ops.cpp
 │     └── change notes          db/change_feed.*          WAL notes for the change feed
 ├── ServerContext               server/redis_commands.h   Database&, Config, ServerStats, Acl, AclLog,
 │     │                                                    ScanCursors, ReplicationState, TlsContext, ReadPool*
 │     └── ScriptEngine          server/scripting.*        LuaJIT state, script cache, function libraries
 ├── Maintenance (thread)        main.cpp                  ObjectCache::run_maintenance, sync_wal, trim_wal
 └── Server                      server/server.*           io_context, listeners, signals
       ├── ReadPool              server/read_pool.*        Database::load_into_cache off the I/O threads
       ├── ReplicaLink           server/replica_link.*     replica side of replication
       ├── RESP sessions         RespParser (resp.*) → execute_redis_command (redis_commands.cpp)
       │                           → CommandSpec table → handlers in cmd_*.cpp → Database
       └── memcache sessions     MemcacheSession (memcache_protocol.*) → Database
```

The dependencies only point down: `RocksStorage` and `ObjectCache` know nothing about each other or about
commands; `Database` knows both but nothing about protocols; the server layer knows `Database` through
`ServerContext`. Two small back-references exist: `RocksStorage` asks an `IndexOwnerResolver` (implemented by
`IndexRegistry`, installed with `set_index_resolver`) who owns an index entry during compaction, and
`ServerContext::read_pool` / `ServerContext::replicaof` are set by the `Server` while it runs.

## The life of a request

A RESP request on the plain port, step by step (the TLS port is the same after the handshake):

1. **Accept.** `Server::accept_loop` accepts the socket, sets `TCP_NODELAY` and spawns `redis_session` on a new
   strand. `redis_session` counts the connection (`ConnectionScope`), creates a `ClientState`, logs it in as the
   default user when that needs no password (`start_session`) and runs `serve_redis`.
2. **Read.** `serve_redis` appends what the socket has (`read_more`, 16 KiB at a time,
   [asio_util.h](../../src/server/asio_util.h)) to the input buffer `in`.
3. **Batch scope.** Each round over the buffer opens a `Database::WriteBatchScope`: every write of the requests
   parsed in this round goes to RocksDB as one write batch (group commit), committed before any reply is sent.
4. **Parse.** `RespParser::parse` ([resp.cpp](../../src/server/resp.cpp)) returns `Ok` with `args` (views into
   `in`), `Incomplete`, or `Error` (the error is replied and the connection closed). It handles multibulk and
   inline requests. Before authentication an incomplete request larger than 16 KiB closes the connection.
5. **Dispatch.** `execute_redis_command` ([redis_commands.cpp](../../src/server/redis_commands.cpp)):
   - counts the command, lower-cases its name and looks it up in the command table (`command_table()`);
   - reloads the connection's ACL user if the ACL version changed (a deleted user closes the connection);
   - refuses everything but `AUTH`, `HELLO` and `QUIT` with `NOAUTH` before login;
   - replies `ERR unknown command ...` or `ERR wrong number of arguments ...` (arity, see
     [adding-a-command.md](adding-a-command.md));
   - for a restricted user, runs `acl_check` ([acl.cpp](../../src/server/acl.cpp)): the command, the
     subcommand, and every key argument found by `for_each_key` with its read/write access;
   - refuses `@write` commands with `READONLY` on a read-only replica, and `@read`/`@write` commands with
     `LOADING` while a full sync is under way;
   - calls the handler (`CommandSpec::fn`) with a `Cmd`, inside a `Database::NonBlockingReads` scope if the
     command is deferrable (see below), and turns exceptions into error replies: `IndexConstraintError` as is,
     `StorageError` as `ERR storage error: ...`, any other `std::exception` as `ERR internal error: ...`.
6. **Execute.** The handler (in `redis_commands.cpp` or a `cmd_*.cpp`) calls `Database`: `read` / `get` /
   `exists` for plain reads, `update` / `put` / `del` for strings, `transact` / `transact_many` for collections,
   `peek` for lock-free collection reads. These look in the `ObjectCache` first and go to `RocksStorage` on a
   miss; writes are staged in the thread's write batch and written to the cache.
7. **Reply.** The handler appends RESP2 or RESP3 to the output buffer through `Reply`. When the output reaches
   1 MiB the round ends early.
8. **Commit and flush.** At the end of the round `WriteBatchScope::commit` writes the batch to RocksDB (one
   `DB::Write`) and releases the key locks it held; if that fails it throws `StorageError`, the session catches
   it and closes the connection without sending the replies. Otherwise `flush` writes the replies.

Three things can interrupt a round:

- **A deferred read.** A deferrable read (`GET`, `MGET`, `EXISTS`, ...; the list is `rerunnable()` in
  `redis_commands.cpp`) that misses the cache throws `Database::WouldBlock` instead of reading the disk. The
  partial reply is rewound, the client is marked `retry_later` with `load_key`, the round commits and flushes,
  the keys of up to 256 following requests are prefetched, and the coroutine waits (`async_load`) for the read
  pool to load the key. Then the same request is parsed and run again. See [concurrency.md](concurrency.md).
- **A blocking read.** `CDC.READ ... BLOCK` with nothing to return sets `retry_later` and `wait_for_writes`; the
  coroutine waits in `wait_for_write` until the next RocksDB write or the deadline, then runs it again.
- **Replication.** `REPLSYNC` sets `start_replication`; after flushing, the connection becomes a replication
  stream (`serve_replica`) and never returns to the request loop.

A memcache request follows the same outline in `Server::memcache_session`: a `WriteBatchScope` per round,
`MemcacheSession::process` per line ([memcache_protocol.cpp](../../src/server/memcache_protocol.cpp)), which
calls `Database` directly. Its ACL checks map each memcache command to a Redis one (`get` to `mget`, `set` to
`set`, `delete` to `del`, ...) and use that command's `CommandSpec` (`permission_error`). Memcache reads are not
deferred to the read pool.

## Startup

`main()` in [main.cpp](../../src/main.cpp), in order:

1. `Config::parse_args`: the optional config file, then `--option value` pairs, then validation. On error it
   prints usage and exits with status 1. The log level is applied.
2. `RocksStorage::open` with a `StorageOptions` built from the config: creates the directory, opens the four
   column families with their options and compaction filters.
3. `ObjectCache` with a `CacheOptions`: reserves the address range of the pool (nothing is committed yet). A
   `cache-max-memory` smaller than one page disables the cache, with a warning.
4. `Database(*storage, cache, cache_on_write, resume_backfills)`: reads the collection version mark from the
   System column family, loads the index definitions (`IndexRegistry`), installs it as the compaction filter's
   owner resolver and, on a primary, resumes the backfills of indexes still in `Building` state. With
   `replicaof` set the database is made read-only and backfills are not resumed (the primary builds the
   indexes).
5. `ServerContext`: applies `requirepass` and the `user` lines to the ACL, and creates the TLS context when
   `tls-port` is set (a bad certificate is fatal).
6. The `Maintenance` thread starts.
7. `Server`: creates the `io_context`s (the control one and one per I/O thread, each kept running by a work guard) and the SIGINT/SIGTERM `signal_set`, the `ReadPool` (if `read-threads > 0`),
   and installs `ServerContext::replicaof`.
8. `Server::start`: binds the enabled listeners (on non-Windows systems with `SO_REUSEADDR`), spawns the accept
   loops, starts following the primary if `replicaof` is set, and arms the signal handler.
9. Logs the summary lines and calls `Server::run`, which returns when the `io_context`s stop.

Any exception escaping this sequence is logged as `fatal: ...` and the process exits with status 1.

## Shutdown

SIGINT or SIGTERM calls `Server::stop`, which stops every `io_context`. `Server::run` joins the extra I/O threads and returns; `main` logs
`stopped` and the objects are destroyed in reverse order of construction:

1. `~Server`: clears `ServerContext::replicaof`, stops and destroys the replica link, unsets
   `ServerContext::read_pool` and cancels every pending `on_next_write` wait (blocked `CDC.READ` connections
   hold timers of the `io_context`). Then its members go: the `ReadPool` finishes the loads under way and drops
   the queued ones (their waiters are told "not cached"), then the `io_context`s, which destroy the suspended
   connection coroutines.
2. `~Maintenance`: stops and joins the maintenance thread.
3. `~ServerContext`, `~Database`: the database sets `stopping_` and joins the backfill threads (a backfill
   checks the flag between keys and resumes after the next start).
4. `~ObjectCache`: releases the pool.
5. `~RocksStorage`: unless `fsync always`, syncs the WAL one last time, then closes RocksDB.

Write batches never outlive a round of a connection, so nothing is pending at that point: every reply a client
received corresponds to a committed write. `tests/run_integration.sh` checks both a `kill -9` and a SIGTERM.

## Pub/Sub connections

A normal connection writes its replies itself, after each round of requests. That does not work for a
subscriber, which must receive messages while it waits for its next request. So the first `SUBSCRIBE` (or
`PSUBSCRIBE`, `SSUBSCRIBE`) gives the connection a `Subscriber` ([pubsub.h](../../src/server/pubsub.h)), an
output queue under its own mutex, and from then on:

- `PubSub` (one per `ServerContext`) maps channels, patterns and shard channels to subscribers, held by
  `weak_ptr`. `PUBLISH` encodes the message once per protocol (RESP2 array or RESP3 push) and appends it to
  each matching subscriber's queue; a subscriber whose queue would pass `Subscriber::kMaxPending` (32 MB) is
  marked overflowed and closed.
- `serve_requests` spawns `pump_output` ([server.cpp](../../src/server/server.cpp)) on the connection's strand.
  The pump is the only writer of the socket from then on: it takes what the queue holds, writes it, and sleeps
  on a timer that `Subscriber::push` wakes (a posted `cancel()`, like the blocking waits). The request loop
  keeps reading, and instead of flushing its replies it appends them to the same queue, so replies and
  messages leave in the order they were queued. An asio stream allows one read and one write in flight at a
  time, which is exactly this split.
- A subscription command first queues the replies written so far (`hand_over` in
  [cmd_pubsub.cpp](../../src/server/cmd_pubsub.cpp)), including its confirmation, and only then registers
  with `PubSub`: a message can never overtake the confirmation.
- When the request loop ends, `serve_redis` unsubscribes everything, closes the subscriber (the pump sends
  what is left) and waits for the pump to finish before the socket is destroyed. When the pump ends first
  (write error, overflow) it closes the socket, which ends the request loop.

## Design principles
## Design principles

- **RocksDB is the source of truth.** Every write reaches RocksDB through a write batch; the object cache is a
  copy that can be dropped at any time (`CACHE CLEAR`, eviction, `cache-max-memory 0`) without losing data.
  Expired keys are removed lazily on read and by the `meta` compaction filter; elements of deleted collections
  by range deletes or the `data` compaction filter.
- **The cache is write-through and never older than the disk.** A write updates RocksDB (through its batch)
  and the cache under the key's lock stripe (`Database::store_locked` and `cache_store`; with
  `cache-on-write no` the cached copy is erased instead). A read that misses fills the cache while holding the
  same stripe (`lock_for_read` + `load_locked`), so a slow reader can never put back a value older than a
  concurrent write. A write batch keeps the stripes of the keys it wrote until it commits, and a replicated
  batch locks its keys and erases them from the cache. A record staged by a batch is visible in the cache
  before the batch is written (the batch is written before its connection sends any reply, and a failed write
  erases those cache entries while the stripes are still held); the elements of small collections are only
  published when the batch commits. See [concurrency.md](concurrency.md#publishing-to-the-cache).
- **Per-key transactions.** A command that changes a collection runs in `Database::transact` (or
  `transact_many` for several keys): it sees its own writes and the earlier writes of its pipeline, writes
  index entries in the same batch, and is rolled back if it throws. String commands use `Database::update`, a
  read-modify-write under the key's lock.
- **Pipeline group commit.** All writes of one round of a connection go to RocksDB as one batch, and only the
  last write of each key is kept. Durability is the same as one write per command: the client gets a reply only
  after the batch reached the WAL.
- **Never wait for the disk on an I/O thread for plain reads.** Deferrable reads that miss the cache are loaded
  by the read pool while the I/O thread serves other connections; a key wanted by several clients is read once.
- **Collections are versioned.** A new collection always gets a new version that is never reused, so deleting
  or overwriting one is O(1) and its old elements are invisible at once (see
  [storage-format.md](storage-format.md)).
- **Derived data is maintained in the same write.** Secondary index entries and the change-feed note of a batch
  are written in the same RocksDB batch as the data they describe (see
  [indexes-internals.md](indexes-internals.md) and [replication-and-cdc-internals.md](replication-and-cdc-internals.md)).

## Where to read next

- [source-tour.md](source-tour.md): every file and what it defines.
- [concurrency.md](concurrency.md): key locks, write batches, the read pool and blocking reads.
- [memory-and-cache.md](memory-and-cache.md): the memory pool and the object cache.
- [storage-format.md](storage-format.md): column families, record and key layouts.
- [indexes-internals.md](indexes-internals.md) and [replication-and-cdc-internals.md](replication-and-cdc-internals.md).
- [building-and-testing.md](building-and-testing.md) and [adding-a-command.md](adding-a-command.md).
