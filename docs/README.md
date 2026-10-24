# datadam documentation

datadam is a persistent key-value store that speaks the **Redis protocol (RESP2 and RESP3)** and the
**memcache text protocol**. RocksDB is the source of truth on disk, with an object cache on a dedicated memory
pool in front of it. On top of Redis-style strings and collections, datadam adds what Redis keeps in memory
only or leaves to modules: data sets larger than RAM, persistent secondary indexes, a RediSearch-compatible
search subset, a JSON type, a change feed read from the write-ahead log, and replication.

## User guide

Start here if you want to run datadam or build an application on it.

| Document | What it covers |
|---|---|
| [Getting started](getting-started.md) | Building on Windows, Linux and Docker; running the server; a first session. |
| [Configuration](configuration.md) | Every option, its default, and which ones `CONFIG SET` can change at run time. |
| [Commands](commands.md) | Reference for every supported command, with the differences from Redis. |
| [Transactions](transactions.md) | `MULTI`/`EXEC` blocks and `WATCH`: what they guarantee. |
| [Pub/Sub](pubsub.md) | Channels, patterns and shard channels; delivery and permissions. |
| [Lua scripting](scripting.md) | `EVAL`, `EVALSHA`, functions (`FUNCTION`, `FCALL`): the API, atomicity, `SCRIPT KILL`, the sandbox. |
| [JSON](json.md) | The JSON type: JSONPath and legacy paths, and the `JSON.*` commands. |
| [Secondary indexes](secondary-indexes.md) | `IDX.*`: compound, ordered indexes over hashes and JSON documents, with `UNIQUE`. |
| [Search](search.md) | `FT.*`: the RediSearch-compatible subset (TAG and NUMERIC fields). |
| [Change feed](change-feed.md) | `CDC.READ`: every committed change, in order, read from the WAL. |
| [Replication](replication.md) | Read-only replicas over the Redis protocol, and promotion. |
| [Security](security.md) | Passwords, ACL users, TLS and client certificates. |
| [memcache](memcache.md) | The memcache text protocol, sharing the keyspace with Redis clients. |
| [Operations](operations.md) | Durability, memory sizing, monitoring with `INFO`, backups and troubleshooting. |
| [Benchmarks](benchmarks.md) | Measured results against Redis and Kvrocks, and how to reproduce them. |

## Developer guide

Start here if you want to change datadam itself.

| Document | What it covers |
|---|---|
| [Architecture](dev/architecture.md) | Threads, the life of a request, components, startup and shutdown. |
| [Source tour](dev/source-tour.md) | What each directory and file holds, and where to look for common tasks. |
| [Storage format](dev/storage-format.md) | Column families, key and record encodings, versions and compaction filters. |
| [Concurrency](dev/concurrency.md) | Key locks, per-key transactions, pipeline group commit and the read pool. |
| [Memory and cache](dev/memory-and-cache.md) | Page arena, slab allocator, object cache, admission filter and cached collections. |
| [Index internals](dev/indexes-internals.md) | Index entry layout, maintenance on every write, query planning and `FT.*` execution. |
| [Replication and CDC internals](dev/replication-and-cdc-internals.md) | WAL retention and decoding, and the replication protocol. |
| [Building and testing](dev/building-and-testing.md) | Toolchain, presets, Docker images and every test suite. |
| [Adding a command](dev/adding-a-command.md) | A step-by-step guide, from the handler to the tests. |

## Feature overview

- **Protocols.** RESP2 and RESP3 (`HELLO 3`) on one port, an optional TLS port, and memcache text on
  another. Both protocols share one keyspace.
- **Data types.** Strings with TTLs, hashes, sets, sorted sets, lists and JSON documents, with
  transactions (`MULTI`/`EXEC`/`WATCH`), blocking list and sorted set commands, and Pub/Sub.
- **Storage.** Every write goes to RocksDB before it is acknowledged. The fsync policy is configurable, and
  the writes of a pipeline are committed together as one batch. Data sets can be much larger than memory.
  Reads that miss the cache are served by a pool of reader threads, so they do not block the network threads.
- **Indexes.** `IDX.*` indexes are compound and ordered, like a B-tree index in a relational database.
  `FT.*` indexes give each field its own index and take RediSearch query syntax. Both are written in the same
  batch as the data, so an index never disagrees with the data after a crash.
- **Change feed and replication.** Both read the RocksDB write-ahead log, which datadam keeps for a
  configurable time.
- **Security.** Redis-style ACL users, with per-command and per-key-pattern permissions, and TLS with client
  certificates.

The [project README](../README.md) has the same material in a single page, plus the roadmap.
