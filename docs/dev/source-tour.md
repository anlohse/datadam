# Source tour

A directory-by-directory, file-by-file map of `src/`, `tests/` and `bench/`, with the main types each file
defines, followed by a list of where to look for common tasks. For how the pieces fit together, start with
[architecture.md](architecture.md).

## Contents

- [Top level](#top-level)
- [src/main.cpp](#srcmaincpp)
- [src/util](#srcutil)
- [src/config](#srcconfig)
- [src/mem](#srcmem)
- [src/cache](#srccache)
- [src/storage](#srcstorage)
- [src/db](#srcdb)
- [src/index](#srcindex)
- [src/json](#srcjson)
- [src/server](#srcserver)
- [tests](#tests)
- [bench](#bench)
- [Where to look for common tasks](#where-to-look-for-common-tasks)

## Top level

| File | Purpose |
|---|---|
| [CMakeLists.txt](../../CMakeLists.txt) | Targets `datadam_core` (static library with everything in `src/` but `main.cpp`), `datadam` (the server) and `datadam_tests` (GoogleTest, when `DATADAM_BUILD_TESTS` is on). Compiler flags. |
| [CMakePresets.json](../../CMakePresets.json) | Configure presets `windows` (MSVC, Ninja Multi-Config) and `linux` (Unix Makefiles, Release); build and test presets. |
| [vcpkg.json](../../vcpkg.json) | Dependencies: asio, rocksdb (lz4, zstd), gtest, nlohmann-json, openssl; pinned `builtin-baseline`. |
| [Dockerfile](../../Dockerfile) | Multi-stage Linux build: `toolchain`, `deps`, `build`, `test`, `runtime`. |
| [datadam.conf](../../datadam.conf) | Sample configuration with every option commented. |
| [README.md](../../README.md) | Project overview. |

See [building-and-testing.md](building-and-testing.md) for how they are used.

## src/main.cpp

[main.cpp](../../src/main.cpp) parses the configuration, builds `RocksStorage`, `ObjectCache`, `Database`,
`ServerContext` and `Server`, and runs until a signal. It also defines `Maintenance`, the background thread for
cache eviction, the periodic WAL sync and `trim_wal` (see [architecture.md](architecture.md#threads)).

## src/util

Small, dependency-free helpers.

| File | Defines |
|---|---|
| [clock.h](../../src/util/clock.h) | `unix_time_ms()` (wall clock, used for TTLs) and `steady_time_ms()` (monotonic, used for cache bookkeeping and deadlines). |
| [hash.h](../../src/util/hash.h) | `hash_bytes()`: a fast 64-bit non-cryptographic hash (8-byte blocks folded with the murmur3 finalizer, `mix64`). Used for cache shards and buckets, key lock stripes and the admission filter. |
| [latency_histogram.h](../../src/util/latency_histogram.h) | `LatencyHistogram`: lock-free microsecond histogram (four buckets per power of two) and its RAII `Timer`. Feeds `INFO latency`. |
| [per_thread.h](../../src/util/per_thread.h) | `PerThread<Block>`: counters kept per thread (each writes only its own block with `bump()`, a plain load and store), added up by readers. Used for `ThreadStats` (commands, errors, traffic) and the keyspace hits and misses. |
| [process.h](../../src/util/process.h), [process.cpp](../../src/util/process.cpp) | `process_rss_bytes()`, for `INFO memory`. |
| [log.h](../../src/util/log.h), [log.cpp](../../src/util/log.cpp) | `LogLevel`, `log_debug/info/warn/error` with `std::format` strings; one line per message to stderr. |
| [sha256.h](../../src/util/sha256.h), [sha256.cpp](../../src/util/sha256.cpp) | `sha256_hex()`, for ACL passwords. |
| [spin.h](../../src/util/spin.h) | `cpu_relax()` and `spin_try_lock()` (2000 `try_lock` attempts before giving up), used by the key locks. |
| [strings.h](../../src/util/strings.h), [strings.cpp](../../src/util/strings.cpp) | `to_lower`, `iequals`, strict number parsers (`parse_int64`, `parse_uint64`, `parse_double`, `parse_long_double`), `format_double`, `parse_size` ("512mb"), `parse_duration` ("5m"), `format_size`, `glob_match` (Redis glob), `split_ws`. |

## src/config

| File | Defines |
|---|---|
| [config.h](../../src/config/config.h), [config.cpp](../../src/config/config.cpp) | `Config` (every option with its default), `FsyncPolicy`, `WritePath`; `Config::set`, `load_file`, `parse_args`, `validate`, `dump`. Option names are the strings in `Config::set`. |

What `CONFIG SET` may change at run time is decided in [server_context.cpp](../../src/server/server_context.cpp)
(`runtime_settings()`), not here.

## src/mem

The memory pool behind the object cache. Details in [memory-and-cache.md](memory-and-cache.md).

| File | Defines |
|---|---|
| [page_arena.h](../../src/mem/page_arena.h), [page_arena.cpp](../../src/mem/page_arena.cpp) | `PageArena`: one reserved address range split into pages, committed lazily and decommitted on release beyond the minimum; `PageMeta` per page plus live-chunk bitmaps. |
| [size_classes.h](../../src/mem/size_classes.h), [size_classes.cpp](../../src/mem/size_classes.cpp) | `SizeClasses`: geometric chunk sizes (factor 1.25, 8-byte aligned, at most 64 classes). |
| [slab_allocator.h](../../src/mem/slab_allocator.h), [slab_allocator.cpp](../../src/mem/slab_allocator.cpp) | `SlabAllocator`: per-shard allocator that carves arena pages into chunks of one class; returns empty pages to the arena; `live_chunks()` for page steals. |

## src/cache

| File | Defines |
|---|---|
| [object_cache.h](../../src/cache/object_cache.h), [object_cache.cpp](../../src/cache/object_cache.cpp) | `ObjectCache` (sharded, intrusive hash table, per-class LRU, page steal, idle/age/TTL eviction), `CacheOptions`, `CacheStats`, `CacheClassStats`. The 72-byte `Item` header lives here. |
| [admission.h](../../src/cache/admission.h) | `Doorkeeper`: the TinyLFU-style admission filter (two generations of a bloom filter). |

## src/storage

| File | Defines |
|---|---|
| [rocks_storage.h](../../src/storage/rocks_storage.h), [rocks_storage.cpp](../../src/storage/rocks_storage.cpp) | `RocksStorage` (open, get, `multi_get`, put, batches, cursors, snapshots, `scan`, `delete_all`, WAL sync, flush, compaction), `Cf`, `StorageOptions`, `StorageError`, `RocksStorage::Batch`, `Cursor`, `Snapshot`; the change-feed side (`read_wal`, `read_wal_raw`, `decode_batch`, `trim_wal`, `on_next_write`); `WalVisitor`, `IndexOwnerResolver`; the compaction filters `TtlCompactionFilter`, `DataGcFilterFactory`, `IndexGcFilterFactory`; latency histograms. |
| [record.h](../../src/storage/record.h), [record.cpp](../../src/storage/record.cpp) | `ValueType`, `RecordView` (non-owning, what the cache hands out), `Record`, `encode_record` / `decode_record`, `CollectionMeta` (24 bytes) and its codec, `ElementCache` (the cache-only tail of a collection record). |
| [data_keys.h](../../src/storage/data_keys.h) | Key layout of the `data` column family: `data_prefix`, `decode_data_key`, big-endian helpers, `encode_u64`, `sortable_score` / `score_from_sortable`. |

On-disk layouts are documented in [storage-format.md](storage-format.md).

## src/db

| File | Defines |
|---|---|
| [database.h](../../src/db/database.h), [database.cpp](../../src/db/database.cpp) | `Database`: the keyspace. `read`, `get`, `exists`, `update` (with `Database::Op`), `put`, `del`, `mset`, `rename`, `flush_all`, `keys`, `scan_keys`; `transact` / `transact_many` with `KeyTxn` / `KeyTxns`; `peek`; `WriteBatchScope` and the private `PendingBatch`, `TxnRun`, `KeyGuard`; `WouldBlock`, `NonBlockingReads`, `load_into_cache`, `load_many_into_cache`; replication entry points (`apply_replicated`, `clear_for_full_sync`, `promote`); lock and batch metrics. |
| [key_locks.h](../../src/db/key_locks.h) | `KeyLocks`: 4096 striped mutexes under a writer-preferring world lock (`lock_world` for scripts and `EXEC` blocks that need everything), `KeyLocks::Multi` (several stripes in ascending order), `lock_keys`, `lock_all`. |
| [key_watch.h](../../src/db/key_watch.h) | `KeyWatch`: who is told about writes to a key (`Watcher` for WATCH, `Waiter` for blocking commands); `WatchedKeys`, a connection's WATCH list. |
| [element_set.h](../../src/db/element_set.h), [element_set.cpp](../../src/db/element_set.cpp) | `ElementSet`: all elements of a small collection, read in place from the cached encoding plus an ordered map of changes. |
| [index_ops.cpp](../../src/db/index_ops.cpp) | `Database` methods for secondary indexes: `create_index`, `drop_index`, backfill (`start_backfill`, `run_backfill`), `query_index`, index entry helpers, JSON index entries and UNIQUE checks. |
| [search_ops.cpp](../../src/db/search_ops.cpp) | `Database` methods for `FT.*`: `search` (set operations over index scans), `count_search_entries`, `tag_values`, `search_keys`. |
| [change_feed.h](../../src/db/change_feed.h), [change_feed.cpp](../../src/db/change_feed.cpp) | `Change`, `ChangeBatch`, `ChangeRead`, `read_changes` (WAL to logical changes), and the per-batch WAL note (`change_note`, `add_change_note_type`). |

## src/index

Definitions and query parsing; the code that reads and writes entries is in `src/db/`. See
[indexes-internals.md](indexes-internals.md).

| File | Defines |
|---|---|
| [index_def.h](../../src/index/index_def.h), [index_def.cpp](../../src/index/index_def.cpp) | `IndexDef` (fields, prefixes, source hash/JSON, kind compound/search, UNIQUE; encoding, tuples, entry keys and values), `IndexFieldType`, `IndexState`, `IndexSource`, `IndexKind`, `IndexConstraintError`, value encodings (`encode_index_value`, `tag_bound`, `prefix_end`, ...). |
| [index_registry.h](../../src/index/index_registry.h), [index_registry.cpp](../../src/index/index_registry.cpp) | `IndexRegistry` (definitions persisted in the System column family, `matching`, `covers`, `reload`) and its `Index` (state, backfill progress, `unique_mu`). Implements `IndexOwnerResolver`. |
| [index_query.h](../../src/index/index_query.h) | `IndexCondition`, `IndexQuery`, `IndexHit`, `IndexQueryResult` for `IDX.QUERY` / `IDX.COUNT` / `IDX.GET`. |
| [search_query.h](../../src/index/search_query.h), [search_query.cpp](../../src/index/search_query.cpp) | `SearchNode`, `SearchOptions`, `SearchResult`, `parse_search_query` (RediSearch query syntax for TAG and NUMERIC fields). |

## src/json

| File | Defines |
|---|---|
| [json.h](../../src/json/json.h), [json.cpp](../../src/json/json.cpp) | `Json` (`nlohmann::ordered_json`), `parse_json` with a depth limit, `json_depth`, `dump_json` (compact or with `JsonFormat`), `json_type_name`. |
| [json_path.h](../../src/json/json_path.h), [json_path.cpp](../../src/json/json_path.cpp) | `JsonPath` (JSONPath `$...` and legacy `.a.b` paths: `parse`, `select`, `select_or_create`, `erase`), `JsonSelector`, `JsonStep`, filter expressions. |

## src/server

Protocols, commands and everything per connection.

| File | Defines |
|---|---|
| [server.h](../../src/server/server.h), [server.cpp](../../src/server/server.cpp) | `Server`: `io_context`, listeners, `accept_loop`, `redis_session`, `redis_tls_session`, `memcache_session`, `serve_redis` (the request loop with write batching, deferred reads, blocking reads), `follow` (REPLICAOF). |
| [asio_util.h](../../src/server/asio_util.h) | `kUseAwaitable`, `read_more`, `flush`, `consume`, `wait_for_write` (waits for the next RocksDB write without holding a thread). |
| [resp.h](../../src/server/resp.h), [resp.cpp](../../src/server/resp.cpp) | `RespParser` (multibulk and inline, with size limits) and `Reply` (RESP2/RESP3 writer, `mark` / `rewind`). |
| [redis_commands.h](../../src/server/redis_commands.h) | `ServerContext`, `ServerStats`, `ClientState`, `Args`, `execute_redis_command`, `uncached_read_keys`, `build_info`, `start_session`, `client_info`. |
| [redis_commands.cpp](../../src/server/redis_commands.cpp) | The command table (`build_command_table`, `command_table`, `find_command`, `all_commands`), `execute_redis_command`, `rerunnable`, `INFO`, and the connection, server, string and keyspace commands (`cmd_get`, `cmd_set`, `cmd_cache`, ...). |
| [command.h](../../src/server/command.h) | What command files share: `Cmd`, `Handler`, `CommandSpec`, `CommandList`, ACL categories (`kCat*`), `KeyRange`, `KeyAccess`, `for_each_key`, `CommandMeta`, `may_read_key`, `reads_all_keys`, `with_collection`, `read_collection`, `normalize_range`, common error strings (`kWrongType`, `kSyntax`, ...), and the `*_commands()` declarations. |
| [command_meta.cpp](../../src/server/command_meta.cpp) | `command_meta()`: ACL categories and key positions of every command. |
| [cmd_hash.cpp](../../src/server/cmd_hash.cpp) | `hash_commands()`: `HSET`, `HGET`, `HGETALL`, `HINCRBY`, `HRANDFIELD`, ... |
| [cmd_set.cpp](../../src/server/cmd_set.cpp) | `set_commands()`: `SADD`, `SMEMBERS`, `SMOVE`, `SINTER*`, `SUNION*`, `SDIFF*`, `SINTERCARD`, ... |
| [cmd_zset.cpp](../../src/server/cmd_zset.cpp) | `zset_commands()`: `ZADD`, `ZRANGE` (all forms), `ZRANK`, `ZPOP*`, `ZREMRANGEBY*`, ... |
| [cmd_list.cpp](../../src/server/cmd_list.cpp) | `list_commands()`: `LPUSH`, `LRANGE`, `LINSERT`, `LMOVE`, `RPOPLPUSH`, ... |
| [cmd_scan.cpp](../../src/server/cmd_scan.cpp) | `scan_commands()`: `SCAN`, `HSCAN`, `SSCAN`, `ZSCAN`. |
| [scan_cursors.h](../../src/server/scan_cursors.h) | `ScanCursors`: numeric cursor ids mapped to saved positions (at most 100,000, least recently used forgotten). |
| [cmd_json.cpp](../../src/server/cmd_json.cpp) | `json_commands()`: the `JSON.*` commands. |
| [cmd_index.cpp](../../src/server/cmd_index.cpp) | `index_commands()`: `IDX.*`. |
| [cmd_search.cpp](../../src/server/cmd_search.cpp) | `search_commands()`: `FT.*`. |
| [cmd_tx.cpp](../../src/server/cmd_tx.cpp) | `transaction_commands()`: `MULTI`, `EXEC`, `DISCARD`, `WATCH`, `UNWATCH`; `runs_in_multi`. |
| [cmd_pubsub.cpp](../../src/server/cmd_pubsub.cpp) | `pubsub_commands()`: the (un)subscribe commands, `PUBLISH`, `SPUBLISH`, `PUBSUB`. |
| [scripting.h](../../src/server/scripting.h), [scripting.cpp](../../src/server/scripting.cpp) | `ScriptEngine`: the LuaJIT state and sandbox, `redis.*` and `cjson`, reply conversion both ways, the script cache, function libraries (stored in the System CF), `SCRIPT KILL`. |
| [cmd_script.cpp](../../src/server/cmd_script.cpp) | `script_commands()`: `EVAL`, `EVALSHA`, the `_RO` variants, `SCRIPT`, `FUNCTION`, `FCALL`; `allowed_while_script_busy`. |
| [admin.h](../../src/server/admin.h), [admin.cpp](../../src/server/admin.cpp) | `Backups` (RocksDB checkpoints for `BACKUP`/`BGSAVE`, with retention), `SlowLog`, `loggable_args` (argument redaction), `ClientEntry`/`ClientRegistry` (what `CLIENT LIST`/`KILL` see) and `Monitors`. |
| [metrics.h](../../src/server/metrics.h), [metrics.cpp](../../src/server/metrics.cpp) | `metrics-port`: `render_prometheus` (the `INFO` numbers, commandstats, errorstats and the latency histograms in the Prometheus text format) and `metrics_http` (`/metrics`, `/health`, `/ready`). `Server::http_session` serves them on the control context. |
| [cmd_admin.cpp](../../src/server/cmd_admin.cpp) | `admin_commands()`: `SAVE`, `BGSAVE`, `BACKUP`, `LASTSAVE`, `SHUTDOWN`, `SLOWLOG`, `MONITOR`, `OBJECT`, `TOUCH`, `MEMORY`. |
| [pubsub.h](../../src/server/pubsub.h), [pubsub.cpp](../../src/server/pubsub.cpp) | `Subscriber` (a subscribed connection's output queue) and `PubSub` (channels, patterns and shard channels to subscribers). |
| [cmd_cdc.cpp](../../src/server/cmd_cdc.cpp) | `cdc_commands()`: `CDC.INFO`, `CDC.READ` (with `BLOCK`). |
| [cmd_acl.cpp](../../src/server/cmd_acl.cpp) | `acl_commands()`: `ACL` and its subcommands. |
| [cmd_repl.cpp](../../src/server/cmd_repl.cpp) | `replication_commands()`: `REPLICAOF` / `SLAVEOF`, `ROLE`, `REPLCONF`, `REPLSYNC`. |
| [acl.h](../../src/server/acl.h), [acl.cpp](../../src/server/acl.cpp) | `AclUser` (rules, passwords, command bitmap, key patterns), `Acl` (the users), `AclLog`, `AclDenial`, `acl_check`, `acl_categories`. |
| [memcache_protocol.h](../../src/server/memcache_protocol.h), [memcache_protocol.cpp](../../src/server/memcache_protocol.cpp) | `MemcacheSession`: the memcache text protocol over the same keyspace, with ACL checks through Redis command equivalents. |
| [read_pool.h](../../src/server/read_pool.h), [read_pool.cpp](../../src/server/read_pool.cpp) | `ReadPool`: threads that load uncached keys for deferred reads, with single flight and optional MultiGet batching. |
| [replication.h](../../src/server/replication.h), [replication.cpp](../../src/server/replication.cpp) | The replication protocol (described in the header), System keys `repl:*`, `ReplicationState` (INFO replication, ROLE). |
| [replica_link.h](../../src/server/replica_link.h), [replica_link.cpp](../../src/server/replica_link.cpp) | `ReplicaLink` (replica side: connect, sync, apply, ack, reconnect) and `serve_replica` (primary side: snapshot or WAL stream). |
| [server_context.cpp](../../src/server/server_context.cpp) | `ServerContext` construction (ACL, TLS), `start_session`, `set_config` (CONFIG SET, all or nothing), `rewrite_config` (CONFIG REWRITE). |
| [tls.h](../../src/server/tls.h), [tls.cpp](../../src/server/tls.cpp) | `TlsContext` (OpenSSL context from the `tls-*` settings), `verified_peer_cn`. |

## tests

GoogleTest suites, all linked into `datadam_tests`, plus scripts. What each covers is detailed in
[building-and-testing.md](building-and-testing.md#unit-tests).

| File | Covers |
|---|---|
| [strings_test.cpp](../../tests/strings_test.cpp) | Size, duration and number parsing, glob matching, `LatencyHistogram`. |
| [slab_allocator_test.cpp](../../tests/slab_allocator_test.cpp) | `SizeClasses`, `SlabAllocator`, `PageArena`. |
| [object_cache_test.cpp](../../tests/object_cache_test.cpp) | `ObjectCache`: eviction, page steal, idle/age/TTL, admission. |
| [element_set_test.cpp](../../tests/element_set_test.cpp) | `ElementSet` against a `std::map`. |
| [resp_test.cpp](../../tests/resp_test.cpp) | `RespParser` and `Reply`. |
| [database_test.cpp](../../tests/database_test.cpp) | `Database` directly: persistence, cache, `update`, CAS, TTL, write batches, MultiGet loading. |
| [collections_test.cpp](../../tests/collections_test.cpp) | Collections through `execute_redis_command`: versions, compaction, rename, transactions, cached small collections. |
| [index_test.cpp](../../tests/index_test.cpp) | `IDX.*`: queries, maintenance, backfill, UNIQUE, repair, concurrency. |
| [json_test.cpp](../../tests/json_test.cpp) | JSON paths, `JSON.*` commands, JSON indexes. |
| [search_test.cpp](../../tests/search_test.cpp) | `FT.*`, search index definitions and query parsing. |
| [ops_test.cpp](../../tests/ops_test.cpp) | Pending cache items, `BACKUP`/`BGSAVE` and restoring a backup, `SLOWLOG`, `OBJECT`/`TOUCH`/`MEMORY`; on a real server `CLIENT LIST`/`KILL`, `MONITOR`, `SHUTDOWN`. |
| [realtime_test.cpp](../../tests/realtime_test.cpp) | `MULTI`/`EXEC`/`WATCH` and Pub/Sub through `execute_redis_command`; blocking commands, message delivery and transactions on a real server. |
| [admin_test.cpp](../../tests/admin_test.cpp) | SCAN family, CONFIG SET, AUTH, deferred reads and the read pool. |
| [acl_test.cpp](../../tests/acl_test.cpp) | ACL users and permissions, memcache permissions, CONFIG REWRITE, ACL LOG. |
| [cdc_test.cpp](../../tests/cdc_test.cpp) | `CDC.INFO` / `CDC.READ`, BLOCK, WAL retention. |
| [replication_test.cpp](../../tests/replication_test.cpp) | Two real servers in one process: full sync, stream, restart, promotion. |
| [tls_test.cpp](../../tests/tls_test.cpp) | A real server with TLS and generated certificates. |
| [integration_test.py](../../tests/integration_test.py) | End-to-end RESP and memcache checks against a running server (standard library only). |
| [run_integration.sh](../../tests/run_integration.sh) | Starts a server, runs `integration_test.py`, then `kill -9` + restart + verify, then SIGTERM. |
| [redis_cli_test.sh](../../tests/redis_cli_test.sh) | Every command through the real `redis-cli`; phases `commands`, `write`, `verify`. |
| [docker_redis_cli.sh](../../tests/docker_redis_cli.sh) | Runs the image and `redis_cli_test.sh` in Docker: plain, persistence cycles, TLS, a replica. |

## bench

| File | Purpose |
|---|---|
| [memtier.sh](../../bench/memtier.sh) | memtier_benchmark of datadam, Redis and Kvrocks in memory-limited containers; scenarios `fit` and `big`. |
| [load_random.py](../../bench/load_random.py) | Loads the data set with incompressible random values. |
| [report.py](../../bench/report.py) | Turns a results directory into tables. |
| [repro_tail.sh](../../bench/repro_tail.sh) | Reproduces one workload against one image and prints what the server saw. |
| [RESULTADOS.md](../../bench/RESULTADOS.md) | Results and method (Portuguese); raw results in `bench/results/`. |

See [benchmarks.md](../benchmarks.md) for the user-facing summary.

## Where to look for common tasks

| Task | Start in |
|---|---|
| Add or change a Redis command | [adding-a-command.md](adding-a-command.md), then a `cmd_*.cpp` or `redis_commands.cpp`, and [command_meta.cpp](../../src/server/command_meta.cpp) |
| Change how a request is parsed or dispatched | `RespParser` in [resp.cpp](../../src/server/resp.cpp), `execute_redis_command` in [redis_commands.cpp](../../src/server/redis_commands.cpp) |
| Change the connection loop (pipelining, flushing, deferral) | `Server::serve_redis` in [server.cpp](../../src/server/server.cpp) |
| Add a configuration option | `Config` in [config.h](../../src/config/config.h), `Config::set` / `dump` in [config.cpp](../../src/config/config.cpp), `runtime_settings()` and `set_config` in [server_context.cpp](../../src/server/server_context.cpp) if it changes at run time, [datadam.conf](../../datadam.conf), [configuration.md](../configuration.md) |
| Add an `INFO` field | `build_info` in [redis_commands.cpp](../../src/server/redis_commands.cpp) |
| Change locking or write batching | [database.cpp](../../src/db/database.cpp) (`lock_key`, `lock_for_read`, `commit_batch`), [key_locks.h](../../src/db/key_locks.h); read [concurrency.md](concurrency.md) first |
| Change eviction or the memory pool | [object_cache.cpp](../../src/cache/object_cache.cpp), [src/mem/](../../src/mem/); read [memory-and-cache.md](memory-and-cache.md) |
| Change an on-disk layout | [record.h](../../src/storage/record.h), [data_keys.h](../../src/storage/data_keys.h), [index_def.h](../../src/index/index_def.h); read [storage-format.md](storage-format.md) |
| RocksDB options or compaction filters | `column_family_options` and `RocksStorage::open` in [rocks_storage.cpp](../../src/storage/rocks_storage.cpp) |
| Secondary indexes / search | [index_ops.cpp](../../src/db/index_ops.cpp), [search_ops.cpp](../../src/db/search_ops.cpp), `KeyTxn::prepare_indexes` in [database.cpp](../../src/db/database.cpp), [src/index/](../../src/index/) |
| Change feed or replication | [change_feed.cpp](../../src/db/change_feed.cpp), [cmd_cdc.cpp](../../src/server/cmd_cdc.cpp), [replica_link.cpp](../../src/server/replica_link.cpp), [replication.h](../../src/server/replication.h) |
| ACL rules or categories | [acl.cpp](../../src/server/acl.cpp), the `kCat*` enum in [command.h](../../src/server/command.h) |
| memcache protocol | [memcache_protocol.cpp](../../src/server/memcache_protocol.cpp) |
| A new test | the fixture of the closest `tests/*_test.cpp`, and a line in [redis_cli_test.sh](../../tests/redis_cli_test.sh) |
