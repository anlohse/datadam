# Operations

This page is for running datadam in production: what each durability setting guarantees, how to size memory
and the cache, how disk reads are handled, what every `INFO` section reports, how keys expire, how to take
backups, how to deploy with Docker, and what common errors mean.

- [Durability](#durability)
- [Memory and cache sizing](#memory-and-cache-sizing)
- [RocksDB settings](#rocksdb-settings)
- [Disk reads and the read pool](#disk-reads-and-the-read-pool)
- [Monitoring](#monitoring)
- [Expiration](#expiration)
- [Backups](#backups)
- [Docker deployment](#docker-deployment)
- [Troubleshooting](#troubleshooting)

## Durability

Every write goes to RocksDB, and with the default write path every write reaches the RocksDB write-ahead log
(WAL) with a `write` system call before its reply is sent. RocksDB replays the WAL on startup, so a crash of
the process, including `kill -9`, never loses an acknowledged write. What can be lost on a power failure or a
kernel crash depends on `fsync`:

| `fsync` | WAL synced to stable storage | Lost on power loss |
|---|---|---|
| `always` | Before each reply (one sync per write, or per group-committed batch) | Nothing acknowledged |
| `everysec` (default) | Once per second, by the maintenance thread | Up to about the last second |
| `no` | When the operating system flushes its cache | Whatever the OS had not written |

`fsync` can be changed at run time with `CONFIG SET`. On a clean shutdown the WAL is synced whatever the
setting. `INFO persistence` shows the policy in effect.

The `experimental-write-path` option trades durability for speed in benchmarks only (`wal-buffered`,
`no-wal`, `memory`); see [Configuration](configuration.md#experimental). Keep it at `wal` for real data.

### Group commit

With `write-batching yes` (the default), a connection that sends several requests at once (a pipeline) has
the writes of all the requests read together staged in memory and committed to RocksDB as one write batch,
before any of their replies is sent. memcache connections batch the same way.

- **Same durability.** A client gets `OK` only after the batch is in the WAL (and synced, with
  `fsync always`), exactly as with one write per request.
- **Isolation.** While a batch is pending, the lock stripes of the keys it wrote stay held, so no other client
  reads the old value from disk.
- **No deadlocks.** A batch never waits for a lock while holding others: on a busy lock it commits what it has
  first, then waits. These early commits are counted in `write_batch_conflicts`.
- **Early commits.** A batch is also committed before a read in the same pipeline goes to disk, after 1,024
  writes or 4 MB, and before `MSET`, `MSETNX`, `RENAME`, `FLUSHALL`, `KEYS`, `SCAN`, `DBSIZE` and index
  queries, which read or change RocksDB directly.
- **Last write wins.** When a pipeline writes the same key several times, only the last value reaches RocksDB:
  16 `INCR` on one counter become a single put.
- **Failure.** If the batch cannot be written (a full disk, for example), its replies are discarded and the
  connection is closed, so the client never sees `OK` for a write that is not stored. A write that fails
  outside a batch is answered with `ERR storage error: ...`.

`INFO persistence` reports `write_batches` (batches committed), `write_batch_ops` (writes in them) and
`write_batch_conflicts`. With `write-batching no`, each write is its own RocksDB write.

Every command is atomic on its own: a command that fails halfway changes nothing. See
[Concurrency](dev/concurrency.md) for the locking model.

## Memory and cache sizing

datadam never refuses a write for lack of memory: all data is on disk, and memory only holds caches. The
resident memory of the process is roughly:

| Part | Bound | Option |
|---|---|---|
| Object cache (memory pool) | `cache-max-memory`, committed page by page as it fills | `cache-max-memory` |
| RocksDB block cache (data, index and filter blocks) | `rocksdb-block-cache` | `rocksdb-block-cache` |
| RocksDB memtables | `rocksdb-write-buffer` per memtable, up to 4 per column family, 4 column families | `rocksdb-write-buffer` |
| Admission filter | About `cache-max-memory / 256` (2 MB for 512 MB) | `cache-max-memory` |
| Connections, threads, `SCAN` cursors | Small; grows with the number of clients and pipelines | `io-threads`, `read-threads` |

In a container with a memory limit, the operating system's page cache for the SST files also counts against
the limit. The benchmarks gave caches about half of the container limit: a 1 GB pool plus a 256 MB block
cache in a 2 GB container (926 MiB used after loading 1 million 512-byte values), and a 384 MB pool, a
128 MB block cache and 32 MB write buffers in a 1 GB container, with no out-of-memory kill. See
[Benchmarks](benchmarks.md).

### What goes in the object cache

- Objects (key plus value) up to `cache-max-object-size` (4 KB by default). Larger values are always read from
  RocksDB, where `rocksdb-block-cache` keeps recently read blocks.
- Each cached item takes a 72-byte header plus its key and value, rounded up to its size class (classes grow
  by about 25%). `cache_chunk_bytes` against `cache_payload_bytes` in `INFO cache` shows the overhead.
- Collections (hash, set, sorted set, list) of up to 128 elements whose encoded elements fit in
  `cache-max-object-size` are cached whole, so reads like `HGETALL` or `ZRANGE` never touch the disk. Larger
  collections cache only their metadata record (size, TTL), and their elements are read from RocksDB.
- JSON documents are cached like strings.
- Writes populate the cache (`cache-on-write yes`); with `no`, a write removes the cached copy.

Raising `cache-max-object-size` lets larger values and collections stay in memory, but it also raises the pool
page size (four times the largest chunk, rounded up to a power of two, at least 64 KB), which makes moving
pages between size classes coarser.

### Eviction

| When | What leaves |
|---|---|
| The pool is full | The least recently used item. If the oldest item is in another size class, its whole page is emptied and reused (a page steal). |
| The pool is full and the key is new (`cache-admission yes`) | Nothing: the key is not admitted until its second access within a window of about as many keys as the cache holds. A key loaded for a waiting client is always admitted. |
| `cache-max-idle` | Items not read for that long, never shrinking the cache below `cache-min-memory`. |
| `cache-max-age` | Items loaded longer ago than that, even if hot. |
| The key's TTL passed | The item, when it is read or when the maintenance crawl reaches it. |

The maintenance thread runs every `cache-maintenance-interval` (but at least once per second for its other
duties). Each pass releases up to 10,000 idle items per shard and checks a slice of each shard's hash table
for age and TTL.

Signs to look at in `INFO cache` or `CACHE STATS`:

- `cache_hit_ratio` is the share of lookups that hit since startup.
- A steady rise of `cache_evicted_lru` with a low hit ratio means the hot set does not fit: raise
  `cache-max-memory`, or check whether the values exceed `cache-max-object-size` (`cache_rejected_too_large`).
- `cache_rejected_admission` counts first-time keys turned away while the pool was full; with a scan or a bulk
  load it protects the hot keys.
- `cache_alloc_failures` counts items that could not get memory even after eviction; it should stay at or near
  zero.

### Cache commands

| Command | Effect |
|---|---|
| `CACHE STATS` | Settings and counters of the cache, as a map (the fields of `INFO cache` without the `cache_` prefix and without `hit_ratio`). |
| `CACHE CLASSES` | One entry per size class: `chunk_size`, `pages` and `items`. |
| `CACHE CLEAR` | Empties the cache. No data is deleted; the next reads go to disk. |

The pool internals are described in [Memory and cache internals](dev/memory-and-cache.md).

## RocksDB settings

Three RocksDB settings are options: `rocksdb-block-cache` and `rocksdb-write-buffer` (both changeable with
`CONFIG SET`) and `rocksdb-background-jobs`. The rest is fixed in the code:

| Setting | Value |
|---|---|
| Compression | LZ4; ZSTD on the bottommost level |
| Block size | 16 KB |
| Bloom filters | 10 bits per key, partitioned |
| Index | Two-level (partitioned) index |
| Index and filter blocks | In the block cache, in its high-priority half; L0 and top-level blocks pinned |
| Level sizing | `level_compaction_dynamic_level_bytes` |
| Memtables | Up to 4 per column family |
| Background sync of files being written | Every 1 MB |
| RocksDB `LOG` files kept | 5 |

`KEYS`, `SCAN` and index backfills read without filling the block cache, so a scan does not push hot blocks
out of it.

When writes come faster than compaction, RocksDB slows them down (write stalls). They show in `INFO latency`
as a high `storage_write_usec` and in RocksDB's own `LOG` file in `dir`:

```bash
grep -hE "Stalling|stall" /var/lib/datadam/LOG | tail -3
grep -h "Cumulative stall" /var/lib/datadam/LOG | tail -1
```

More `rocksdb-background-jobs` (fixed at startup) gives compaction more threads; a larger
`rocksdb-write-buffer` makes fewer, larger flushes at the cost of memory.

Disk usage can be well above the size of the data while compaction catches up after many overwrites (in the
benchmark with 3.1 GB of values, 10.9 GB at the end of the run of random overwrites), and expired keys take space
until compaction drops them (see [Expiration](#expiration)).

## Disk reads and the read pool

When a key is not in the cache, it has to be read from RocksDB, which takes from hundreds of microseconds to
milliseconds. If the I/O thread waited for the disk, only `io-threads` reads could be in flight, and every
connection on that thread would stall. The read pool takes these reads off the I/O threads:

- **Deferred reads.** `GET`, `MGET`, `EXISTS`, `STRLEN`, `GETRANGE`, `SUBSTR`, `TYPE`, `TTL`, `PTTL`,
  `EXPIRETIME`, `PEXPIRETIME`, `JSON.GET`, `JSON.MGET`, `JSON.TYPE`, `JSON.STRLEN`, `JSON.ARRLEN`,
  `JSON.ARRINDEX`, `JSON.OBJKEYS` and `JSON.OBJLEN` have no side effect. When one of them misses the cache on
  the Redis protocol, RocksDB is first asked to answer from memory only (its memtables, its block cache, and
  the bloom filters there). If it can, which covers keys that do not exist and values whose block is cached,
  the command just goes on, on the I/O thread. Only when the answer needs a file read does the connection
  commit its pending writes, send the earlier replies, and ask the pool for the key. The I/O thread serves
  other connections meanwhile. When the key is in the cache, the command runs again; the order of replies in
  a pipeline is kept.
- **Single flight.** Clients that ask for a key while it is being loaded wait for the same disk read, so a hot
  key that leaves the cache does not cause a burst of identical reads.
- **Pipelines in parallel.** When a read is deferred, the keys of the plain reads further down the pipeline
  (up to 256 requests) are queued as well, so their disk reads overlap.
- **Admission.** A key loaded by the pool always enters the cache, because a client waits for it.
- Keys that do not exist, and values larger than `cache-max-object-size`, do not stay in the cache. Most of
  their reads are answered from RocksDB's memory as above; when one did go to the pool, the command runs
  again reading the disk on the I/O thread.
- **Cost.** In Docker with 50 connections, `GET` of keys that do not exist makes 300,000 requests/s (2 million/s
  with a pipeline of 16), and `GET` of 8 KB values (too large for the object cache, in RocksDB's block cache)
  300,000/s (800,000/s pipelined). Before the memory-only check, both went through the pool: 17,000/s
  pipelined.
- memcache commands and collection reads (`HGET`, `SMEMBERS`, `ZRANGE`, ...) are not deferred: on a miss they
  read the disk on the I/O thread.

`read-threads` (64 by default) is the number of disk reads that can be in flight. In the benchmark with data
three times larger than memory, a pipelined mixed workload made about 66,000 to 72,000 ops/s with 32
threads and 82,000 to 89,000 with 64 or 128 (see [Benchmarks](benchmarks.md)). `read-threads 0` turns the
pool off.

`read-batch N` lets each thread take up to N queued keys and read them with one `MultiGet`. RocksDB reads the
blocks of one SST file in parallel but different files one after another, so with random keys it measured no
faster; the default is 1.

`INFO stats` reports `deferred_reads`, `memory_reads` (misses that RocksDB answered from memory, without the
pool), `read_pool_loads`, `read_pool_batches`, `read_pool_joined` and
`read_pool_queued`, and `INFO latency` reports `read_pool_load_usec` and `storage_multiget_usec`.

## Monitoring

`INFO [section]` returns the sections below, in this order. Without an argument, or with `all`,
`everything` or `default`, it returns all of them. `INFO memory` is answered with the `# Cache` section. An
unknown section name returns an empty reply. Section names are case-insensitive.

| Section | Content |
|---|---|
| `server` | Version, process, ports, uptime |
| `clients` | Connected clients |
| `stats` | Connections, commands, authentication, TLS, read pool counters |
| `cache` (also `memory`) | Object cache settings and counters |
| `persistence` | Durability settings, group commit counters, RocksDB figures |
| `latency` | Latency histograms |
| `replication` | Role, link to the primary, connected replicas |
| `keyspace` | Estimated number of keys |

Counters are totals since startup. `CONFIG RESETSTAT` clears only the latency histograms.

### Server

| Field | Meaning |
|---|---|
| `datadam_version` | datadam version |
| `redis_version` | Redis version reported for compatibility (`7.2.0`) |
| `redis_mode` | Always `standalone` |
| `process_id` | Process id |
| `tcp_port` | `port` (the plain Redis port; `0` when disabled) |
| `memcache_port` | `memcache-port` |
| `io_threads` | `io-threads` |
| `uptime_in_seconds`, `uptime_in_days` | Time since startup |

### Clients

| Field | Meaning |
|---|---|
| `connected_clients` | Open connections of every kind: Redis, TLS, memcache, and replicas streaming from this server |
| `blocked_clients` | Redis connections waiting in a blocking command (`BLPOP`...) or in `CDC.READ ... BLOCK` |
| `pubsub_clients` | Redis connections with at least one Pub/Sub subscription |

### Stats

| Field | Meaning |
|---|---|
| `total_connections_received` | Connections accepted, every protocol |
| `total_commands_processed` | Redis protocol commands executed (a deferred read counts once) |
| `auth_failures` | Failed `AUTH`/`HELLO ... AUTH` and memcache `auth` attempts |
| `acl_access_denied` | Commands or keys refused by ACL rules |
| `tls_connections` | TLS handshakes completed |
| `tls_handshake_failures` | TLS handshakes that failed or did not finish within 10 s |
| `deferred_reads` | Reads handed to the read pool because their key was not cached and RocksDB needed a file read |
| `memory_reads` | Reads whose key was not cached but RocksDB answered from memory (memtable, block cache, filters), on the I/O thread |
| `read_pool_loads` | Keys loaded by the read pool |
| `read_pool_batches` | Disk reads made by the pool (one key, or a `MultiGet` batch) |
| `read_pool_joined` | Requests that joined a load already under way (single flight) |
| `read_pool_queued` | Keys waiting for a read thread right now |

### Cache

| Field | Meaning |
|---|---|
| `cache_max_memory`, `cache_min_memory`, `cache_max_object_size` | Settings, in bytes |
| `cache_max_idle_ms`, `cache_max_age_ms` | Settings, in milliseconds |
| `cache_page_size` | Pool page size, in bytes |
| `cache_pages_in_use` | Pages holding items |
| `cache_pages_committed` | Pages backed by memory (in use plus kept for reuse) |
| `cache_used_memory` | `cache_pages_in_use × cache_page_size` |
| `cache_chunk_bytes` | Bytes of chunks in use (header, key, value and rounding) |
| `cache_payload_bytes` | Key and value bytes cached |
| `cache_items` | Items cached |
| `cache_hits`, `cache_misses`, `cache_hit_ratio` | Lookups that hit or missed, and the hit share (4 decimals) |
| `cache_rejected_too_large` | Inserts refused because the object exceeds `cache-max-object-size` |
| `cache_rejected_admission` | New keys refused by the admission filter while the pool was full |
| `cache_alloc_failures` | Inserts that could not get memory |
| `cache_evicted_lru` | Items evicted under memory pressure |
| `cache_evicted_idle`, `cache_evicted_age`, `cache_evicted_ttl` | Items released by `cache-max-idle`, `cache-max-age` and expiration |
| `cache_page_steals` | Pages emptied and moved to another size class |

### Persistence

| Field | Meaning |
|---|---|
| `engine` | `rocksdb` |
| `dir` | Data directory |
| `fsync` | Policy in effect |
| `write_batching` | `yes` or `no`, in effect |
| `write_batches`, `write_batch_ops`, `write_batch_conflicts` | Group commits, writes in them, and commits forced by a lock conflict |
| `rocksdb_estimate_num_keys` | RocksDB's estimate of the keys (the `meta` column family: one record per key) |
| `rocksdb_live_sst_bytes` | Size of the live SST files of the `meta` column family (collection elements and index entries live in other column families and are not included) |
| `rocksdb_memtable_bytes` | Memtable bytes of the `meta` column family |
| `rocksdb_block_cache_usage` | Bytes in the shared block cache |
| `rdb_bgsave_in_progress` | 1 while a `BGSAVE` backup runs |
| `rdb_last_save_time` | Unix time of the last successful `SAVE`, `BGSAVE` or `BACKUP` (the start time before any) |
| `rdb_last_bgsave_status` | `ok`, or `err` if the last backup failed (the server log says why) |
| `last_backup_path`, `last_backup_duration_ms` | Where the last successful backup went, and how long the last one took |

For the total disk usage, measure `dir` itself.

### Latency

Each line is a histogram, in microseconds, of where time goes:

```text
storage_get_usec:calls=120344,p50=447,p99=3071,p99.9=6143,p99.99=12287,max=20811
```

A percentile is the upper bound of its histogram bucket, so it is at most 25% above the true value (and never
above `max`).

| Line | What is timed |
|---|---|
| `storage_get_usec` | A RocksDB point read |
| `storage_multiget_usec` | A `MultiGet` of `read-batch` keys |
| `storage_write_usec` | Writing one batch to RocksDB (includes the sync with `fsync always`) |
| `key_lock_wait_usec` | Waiting for a busy key lock to write (only waits that did not succeed at once) |
| `key_read_lock_wait_usec` | Waiting for a busy key lock to read from disk |
| `write_batch_hold_usec` | How long a group-commit batch held its key locks |
| `read_pool_load_usec` | From asking the read pool for a key to the key being cached (only with `read-threads` above 0) |

Clear the histograms with `CONFIG RESETSTAT` before a measurement, then read `INFO latency` after it.
`bench/repro_tail.sh` does exactly that around a benchmark run.

### Replication

On a primary:

| Field | Meaning |
|---|---|
| `role` | `master` |
| `connected_slaves` | Replicas streaming from this server |
| `slave<N>` | `ip=...,port=...,state=...,offset=...,lag=...` per replica: `state` is `sync` during a full sync and `online` afterwards, `offset` the last offset it acknowledged, `lag` seconds since its last acknowledgment |
| `own_replid` | This server's replication id |
| `repl_offset` | Next WAL sequence number |
| `full_syncs_served`, `partial_syncs_served` | Full and continued syncs served to replicas |

On a replica, these come first (and the primary fields above follow):

| Field | Meaning |
|---|---|
| `role` | `slave` |
| `master_host`, `master_port` | The primary |
| `master_link_status` | `up` while streaming, `down` otherwise |
| `master_last_io_seconds_ago` | Seconds since data last came from the primary (`-1` before any) |
| `master_sync_in_progress` | `1` during a full sync |
| `slave_repl_offset` | Offset applied |
| `slave_read_only` | Always `1` |
| `master_replid` | Id of the primary followed |
| `full_syncs`, `partial_syncs` | Full and continued syncs done |

`ROLE` returns the same information in the Redis format. See [Replication](replication.md).

### Keyspace

```text
db0:keys=1000000,expires=0,avg_ttl=0
```

`keys` is RocksDB's estimate, the same number `DBSIZE` returns; it can include expired keys that compaction
has not removed yet. `expires` and `avg_ttl` are always `0`.

### memcache stats

The memcache `stats` command returns `pid`, `uptime`, `time`, `version`, `threads` (`io-threads`),
`curr_connections`, `total_connections`, `curr_items` (the key estimate), `limit_maxbytes`
(`cache-max-memory`), `bytes` (cached payload bytes), `cache_items`, `get_hits`, `get_misses` and `evictions`
(LRU, idle and age evictions of the cache).

### Other sources

- **Server log.** Standard error, with UTC timestamps. `log-level debug` adds connection and per-client errors
  (including why a TLS handshake failed). Replication progress and errors are logged at `info` and `warn`.
- **RocksDB `LOG`.** In `dir`: compactions, flushes and stalls.
- **`CDC.INFO`.** The kept WAL: `first_sequence`, `last_sequence`, `wal_bytes`. See
  [Change feed](change-feed.md).
- **`ACL LOG`.** Refused commands and keys and failed logins. See [Security](security.md).
- **`SLOWLOG`.** Commands that took at least `slowlog-log-slower-than` microseconds (10 ms by default; `0`
  logs everything, `-1` nothing), newest first, up to `slowlog-max-len` (128). `SLOWLOG GET [count]` gives
  for each: id, Unix time, duration in microseconds, arguments, client address and name. Arguments are cut
  as in Redis (32 arguments, 128 bytes each) and passwords are hidden: `AUTH` and `HELLO` show only their
  name, `ACL SETUSER` the user name and `CONFIG SET` nothing past `SET`, followed by `(redacted)`.
  `SLOWLOG LEN` and `SLOWLOG RESET`. The time measured is the command's own work; a blocked command is
  timed per attempt, not for its wait.
- **`MONITOR`.** Streams every command other connections run, after it runs, as
  `+<unix seconds>.<microseconds> [0 <client address>] "command" "arg" ...`, with the same redaction.
  Monitoring costs every command a formatted line per monitor; use it for debugging, not continuously.
- **`CLIENT LIST`.** Every Redis connection: `id`, `addr`, `name`, `age` and `idle` (seconds), `flags` (`N`
  normal, `P` Pub/Sub, `O` monitor, `S` replica, `b` blocked), `sub`, `psub`, `ssub`, the last command
  (`cmd`), `user`, `resp`, `lib-name`, `lib-ver` and `tls`. `CLIENT LIST TYPE normal|pubsub|replica` and
  `CLIENT LIST ID id ...` filter it. memcache connections are not listed.
- **`OBJECT ENCODING`, `MEMORY USAGE`.** What a key holds and about how much space it takes; see
  [Commands](commands.md#keys-and-expiration).
- There is no `LATENCY` command (`INFO latency` has the histograms).

### Slow paths

Most commands are served from memory or with one point read. These are the cases that cost more:

- **Reads that miss the cache.** Deferred to the read pool for the commands listed under
  [Disk reads](#disk-reads-and-the-read-pool); on the I/O thread for memcache and collection reads.
- **Values larger than `cache-max-object-size`.** Always read from RocksDB.
- **Large collections.** Elements are read from RocksDB; `ZRANK`, `ZRANGE` by position, `LINSERT` and `LREM`
  walk up to the position (O(N)).
- **`KEYS`** walks every key on disk; prefer `SCAN` with a `MATCH` that starts with a fixed prefix, which only
  reads that prefix.
- **`FLUSHALL`/`FLUSHDB`** lock every key while they delete everything.
- **`UNIQUE` index writes** commit at once instead of waiting for the rest of the pipeline.
- **`FT.SEARCH`** always counts the total, so it reads every index entry of each field in the query; a
  composite `IDX.*` index reads only the page. See [Search](search.md).
- **Index creation** backfills in the background over the existing keys.

## Expiration

Keys with a TTL are removed in three ways; there is no background cycle that scans the disk for expired keys:

1. **On access.** A read of an expired key on a primary deletes it (and drops a collection's elements in the
   same write). Replicas leave this to the primary.
2. **By compaction.** A compaction filter drops expired records when RocksDB compacts the files that hold them,
   and the elements of collections that no longer exist. Keys that are never read again free their disk space
   this way, as compaction reaches their files.
3. **In the cache.** The maintenance thread releases expired items from the object cache.

Expired keys are never returned to clients, by any command, even before they are removed. Until compaction
reaches them, they still take disk space and are counted by `DBSIZE` and `INFO keyspace`. Expirations done by
compaction do not appear in the change feed; that is why its `set` and `meta` records carry the expiry time.

## Backups

### Online backups

`BACKUP <path>` writes a consistent copy of the whole database to `path` while the server keeps running, and
replies once it is complete:

```text
> BACKUP /var/backups/datadam/2026-10-23
OK
```

It is a RocksDB checkpoint: the memtables are flushed, then every SST file is hard-linked into `path`, so on the
same file system it takes about the time of the flush and almost no space at first (the files are shared with
the live database until compaction replaces them there). On another file system the files are copied. `path`
must not exist (`ERR backup target '...' already exists`); its parent is created. The copy is a complete
database directory: to restore it, start a server with `dir` set to it (or to a copy of it).

`BGSAVE` does the same in the background when `backup-dir` is set: each call writes
`<backup-dir>/<UTC yyyymmdd-hhmmss>` and then removes the oldest of those directories beyond `backup-keep`
(7 by default; 0 keeps them all). A second `BGSAVE` while one runs replies
`ERR Background save already in progress` (`BGSAVE SCHEDULE` replies `Background saving scheduled` and does
nothing). `INFO persistence` shows the progress (`rdb_bgsave_in_progress`, `rdb_last_bgsave_status`,
`last_backup_path`), and `LASTSAVE` the time of the last success. Both settings can change with `CONFIG SET`.

```text
CONFIG SET backup-dir /var/lib/datadam-backups backup-keep 14
BGSAVE
```

Hard links make a backup cheap, but they are not a copy of the bytes on another disk: for protection against
losing the disk, copy the backup directory elsewhere (it does not change once written).

Without `backup-dir`, `BGSAVE` behaves like `SAVE`: both commit the connection's pending writes and flush every
memtable into SST files, then reply. A flush shortens WAL replay on the next start but is not needed for
durability. `CONFIG GET save` returns an empty value and `CONFIG GET appendonly` returns `no`.

### Other ways

- **Stop and copy.** Stop the server cleanly (`SIGTERM`), copy the whole `dir`, start it again. A cleanly
  closed directory is a complete RocksDB database; start a server on the copy to restore it.
- **From a replica.** Run a replica (the primary needs `wal-retention`), stop the replica, copy its `dir`,
  and start it again. It continues from its stored offset without a full sync as long as the primary still
  keeps the WAL from that point. The primary keeps serving the whole time. See [Replication](replication.md).
- **Atomic file-system snapshot.** A snapshot that captures the whole `dir` at one instant (LVM, ZFS, a cloud
  volume snapshot) is equivalent to a crash at that instant, and the server recovers from it the way it
  recovers from `kill -9`, by replaying the WAL.

Copying the files of a running server with `cp`, `rsync` or similar is not safe: RocksDB keeps writing,
compacting and deleting files during the copy. Copy a `BACKUP` directory instead.

### Stopping the server

`SHUTDOWN` stops the server cleanly, like `SIGTERM`: no reply, the connections close, and the WAL is synced.
`SHUTDOWN SAVE` flushes the memtables first; `NOSAVE`, `NOW` and `FORCE` are accepted; `SHUTDOWN ABORT` replies
`ERR No shutdown in progress.`. It is in `@admin` and `@dangerous`.

The change feed (`CDC.READ`) streams every change in order and can feed an external copy, but a consumer has
to start from a full copy of the data; see [Change feed](change-feed.md).

## Docker deployment

The runtime image is described in [Getting started](getting-started.md#docker). In production:

- **Data volume.** Mount a volume at `/var/lib/datadam`. Without one, Docker creates an anonymous volume that
  is easy to lose.
- **Configuration.** The image starts the server with `/etc/datadam/datadam.conf --dir /var/lib/datadam`; add
  overrides after the image name, or provide your own file. The server runs as the `datadam` user and owns
  `/etc/datadam`, so `CONFIG REWRITE` can replace the file. `CONFIG REWRITE` writes a temporary file in the
  same directory and renames it over the original, so mount a volume or a host directory on `/etc/datadam`
  (writable by the container's `datadam` user), not a single file: renaming over a file that is itself a mount
  point fails. A named volume mounted there for the first time is filled with the image's `datadam.conf`.
- **Memory limit.** Set the container limit above `cache-max-memory + rocksdb-block-cache` plus the memtables
  and some headroom, remembering that the page cache of the SST files counts against the limit (see
  [Memory and cache sizing](#memory-and-cache-sizing)).
- **Ports.** The image exposes 6379 and 11211. Publish `tls-port` too when it is used.
- **Stopping.** `docker stop` sends `SIGTERM`; the server is the container's main process and shuts down
  cleanly. A `docker kill` loses no acknowledged write either (see [Durability](#durability)).
- **Logs.** The server log goes to standard error, so `docker logs` shows it. RocksDB's `LOG` is in the data
  volume.

```bash
docker volume create datadam-data
docker volume create datadam-conf
docker run -d --name datadam --memory 4g \
  -p 6379:6379 -p 6380:6380 \
  -v datadam-data:/var/lib/datadam -v datadam-conf:/etc/datadam \
  datadam --cache-max-memory 1536mb --rocksdb-block-cache 512mb --memcache-port 0
```

## Troubleshooting

### `LOADING datadam is loading the dataset from its primary`

A replica is doing a full sync: it dropped its data and is applying a snapshot of the primary. Reads and
writes of data get this error until the sync ends (`master_sync_in_progress:1` in `INFO replication`). If
the link breaks during a full sync, the replica reconnects and starts the full sync again, and the error lasts
until one completes. Check `master_link_status` and the replica's log (`replication: ...` lines).

### `READONLY You can't write against a read only replica.`

The server is a replica (`replicaof` in its configuration, or a `REPLICAOF` command). memcache clients get
`SERVER_ERROR read only replica`. Write to the primary, or promote the replica with `REPLICAOF NO ONE`. If a
promoted server becomes a replica again after a restart, its configuration file still has a `replicaof` (or
`slaveof`) line; run `CONFIG REWRITE` after the promotion (it removes the line), or edit the file.

### A replica does not connect

The replica logs the reason as `replication: ...` and retries. Common ones:

- `the primary refused REPLSYNC: -ERR replication needs the WAL kept on the primary: set wal-retention`:
  set `wal-retention` on the primary (it needs a restart).
- `the primary refused AUTH` or `NOAUTH`: set `masterauth` (and `masteruser`) on the replica; the user needs
  permission to run `PING`, `REPLCONF` and `REPLSYNC`.
- Repeated full syncs: the replica falls behind the kept WAL. Raise `wal-retention` (and
  `wal-retention-size`) on the primary so the WAL covers at least the time of a full sync.

### `NOAUTH Authentication required.`

`requirepass` is set or the default user has no access; authenticate first. Before authenticating, only
`AUTH`, `HELLO` and `QUIT` are accepted, and a request larger than 16 KB closes the connection. See
[Security](security.md).

### Running out of memory

datadam does not refuse writes when memory is short; the process can only be killed by the operating system
or the container runtime. If that happens, lower `cache-max-memory`, `rocksdb-block-cache` or
`rocksdb-write-buffer` (the last two can be lowered at run time with `CONFIG SET`), or raise the limit. A
container that holds a large data set also needs room for the page cache.

### Disk full or write errors

Writes fail with `ERR storage error: ...`, or, inside a pipeline batch, the connection is closed without the
replies. Free space or grow the volume. Common causes of disk growth:

- **Kept WAL.** With `wal-retention` set, the kept WAL grows as fast as data is written, up to
  `wal-retention-size` (1 GB by default; `0` means no size bound). `CDC.INFO` shows `wal_bytes`; the files are
  in `dir/archive/`.
- **Compaction lag** after heavy random overwrites, and expired keys not yet compacted (see
  [RocksDB settings](#rocksdb-settings) and [Expiration](#expiration)).

### `ERR the change feed is off: set wal-retention ...`

`CDC.READ` needs `wal-retention` above 0 and `experimental-write-path wal` or `wal-buffered`. A consumer that
asks for a sequence that is no longer kept gets `ERR sequence N is no longer in the kept WAL (the oldest is
M)` and has to start again from a full copy.

### The server does not start

- `datadam: ...` followed by the usage line: a configuration error; the message names the file and line or the
  option.
- `fatal: ...` in the log: the server could not open something, for example a port already in use, a `bind`
  address that is not a local IP address, a TLS file that does not load, or a data directory in use by another
  process (RocksDB's `LOCK` file).
- `object cache disabled (cache-max-memory smaller than one page)`: a warning, not an error; raise
  `cache-max-memory` to enable the cache.

### Slow requests or high tail latency

Clear the histograms with `CONFIG RESETSTAT`, reproduce the load, and read `INFO latency`, `INFO stats` and
`INFO cache`:

- High `storage_get_usec` with a low `cache_hit_ratio`: the reads go to disk. Grow the cache or the block
  cache, or `read-threads` if `read_pool_queued` stays high.
- High `storage_write_usec`: RocksDB write stalls or slow syncs; check the RocksDB `LOG` and `fsync`.
- High `key_lock_wait_usec` or `write_batch_conflicts`: many clients writing the same keys.

`bench/repro_tail.sh` shows how the benchmarks collected these numbers.

### TLS tests skipped, or Docker builds that cannot download

Antivirus software that intercepts TLS (Avast does, even on 127.0.0.1) breaks the end-to-end TLS tests: the
tests detect a server certificate issued by someone else and skip with that reason. They run in Docker. The
same kind of interception breaks the downloads of a Docker build; pass the interceptor's root certificate as
the `extra_ca` build secret (see [Getting started](getting-started.md#docker)).
