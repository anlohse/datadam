# Changelog

What changed in each release of datadam. The format follows [Keep a Changelog](https://keepachangelog.com),
the versions [Semantic Versioning](https://semver.org); what a version promises, and how to upgrade, is in
[docs/releases.md](docs/releases.md).

## [Unreleased]

## [0.3.0] - 2026-11-24

Compatibility with Redis clients, measured by running client libraries' own test suites against datadam: what
they found is fixed, and the commands they use most that datadam lacked are added. The storage format is
unchanged (format 1): a 0.2 data directory opens as it is, and 0.2 can open one written by 0.3.

### Added

- Client libraries' own test suites run against datadam (`tests/clients/`, the Clients workflow: nightly against
  `:edge`, on demand, on pull requests labelled `clients`, and before every release): redis-py 8.1.0 (sync and
  asyncio, RESP2 and RESP3) and node-redlock 5.0.0-beta.2. What datadam does not implement is listed with the
  reason in `tests/clients/redis-py.skip`; see [docs/clients.md](docs/clients.md).
- `COMMAND` and `COMMAND INFO` describe every command as Redis 7 does (arity, flags, key positions, ACL
  categories, key specifications), and `COMMAND GETKEYS` returns a command line's keys.
- `INFO` takes several sections, has `arch_bits` in `server` and a `cluster` section (`cluster_enabled:0`).
- `CLIENT LIST` and `CLIENT INFO` show `laddr`; `CLIENT KILL LADDR`.
- `COPY`, `RANDOMKEY`, `LCS` and `RESET`.
- `ZUNION`, `ZINTER`, `ZDIFF`, `ZUNIONSTORE`, `ZINTERSTORE`, `ZDIFFSTORE`, `ZINTERCARD`, `ZRANGESTORE` and
  `ZRANDMEMBER`; the operations across keys take sets too, as in Redis.

### Security

From a review of the network input, authentication and ACLs, the Lua sandbox and the data commands:

- **Lua: bytecode.** `EVAL`, `SCRIPT LOAD` and `FUNCTION LOAD` accepted LuaJIT bytecode, which LuaJIT does not
  verify: a user allowed to run scripts could read and write the server's memory, and run code in it. Only
  source text is loaded now, and `string.dump` is gone.
- **Lua:** `KEYS` and `ARGV` stayed set after a script, and a function (`FCALL`) of another user could read
  them; `FUNCTION LOAD` ran the library's top-level code with no time limit (a loop hung every script, and then
  the server) and now stops it after 500 ms; `redis.status_reply` with CR or LF forged replies;
  `table.insert` could store into the read-only shared tables.
- **ACL:** `SET ... GET`, `JSON.NUMMULTBY` and `JSON.TOGGLE` reply what the key held, but needed only write
  access (`%W~`); rules for the subcommands of `SCRIPT`, `FUNCTION`, `SLOWLOG`, `OBJECT`, `MEMORY` and
  `PUBSUB` (`-script|flush`) were accepted and covered nothing; a user deleted and created again kept the
  old user's connections logged in; `FT.TAGVALS` returned values of keys the user may not read, and a
  `UNIQUE` clash named such a key.
- **Resources:** a client blocked (`BLPOP`, `CDC.READ BLOCK`) that sent more than 1 MB, or that went away during
  `CDC.READ BLOCK`, kept its connection forever, and could not be killed; so did a subscriber that ended while
  not reading what was queued for it. TLS handshakes in progress were not bounded by `maxclients`. memcache
  values were not bounded by `client-query-buffer-limit`, nor replication acknowledgments at all.
- **Costly commands:** nested JSONPath filters (`$..[?($..[?($..x)])]`) could take hours of CPU; their work
  is bounded now. `JSON.GET INDENT` could build a reply of hundreds of GB, `SRANDMEMBER`, `HRANDFIELD` and
  `ZRANDMEMBER` with a negative count one of TBs: both stop at 512 MB.
- A command that failed midway (`JSON.MGET` with a path over the limits) could leave a partial reply on the
  connection; only the error is sent now.
- OpenSSL 3.6.5 (an overlay port in `vcpkg-ports/`), from 3.6.1: the fixes of 3.6.2 to 3.6.5, among them the
  TLS handshake buffer allocation.
- The CI workflows' actions are pinned to commits, and Dependabot proposes their updates.

### Fixed

- `AUTH` and `HELLO` were checked against the user's command permissions: a user without them could not log in
  again as another.
- A subcommand datadam does not have was refused as a permission error (`NOPERM`) and logged in `ACL LOG`; it is
  now refused as unknown, before permissions are checked.
- ACL rules naming a Redis command datadam does not have (`+cluster`) were refused; they now load and cover
  nothing.
- `PUBSUB NUMSUB` and `SHARDNUMSUB` replied a map in RESP3; they reply a flat array, as Redis does.
- `CONFIG RESETSTAT` reset only the latency histograms; it now resets the `INFO stats` counters,
  `commandstats` and `errorstats` too.
- `CLIENT REPLY OFF` and `SKIP` were accepted, but every reply was still sent.
- `DBSIZE` (RocksDB's estimate of the keys) still counted the keys `FLUSHALL` deleted, until compaction;
  `FLUSHALL` now compacts the key records before it replies.

## [0.2.0] - 2026-11-15

The first published release: a container image, a Linux tarball and a Debian/Ubuntu package, with checksums,
SBOMs and build provenance. Version 0.1.0 was never released; images of `main` were published as
`ghcr.io/anlohse/datadam:sha-<commit>`.

### What datadam is

- A persistent key-value store speaking Redis (RESP2 and RESP3) and memcache (text), with RocksDB as the source
  of truth and an object cache on a memory pool of its own in front: data sets larger than memory.
- Strings, hashes, sets, sorted sets and lists; a JSON type compatible with RedisJSON; persistent secondary
  indexes (`IDX.*`) on hashes and JSON, with `UNIQUE`; RediSearch-compatible `FT.*` over `TAG` and `NUMERIC`
  fields; Lua scripting (`EVAL`, Redis 7 functions); transactions; Pub/Sub; a change feed (`CDC.*`).
- Replication with Sentinel failover, history continuation after a promotion, and `promotion-handoff` for
  planned failovers that lose no write acknowledged with `WAIT`.
- ACLs, TLS, protected mode, Prometheus metrics (`metrics-port`), online backups (`BACKUP`).
- Tested on AWS in seven phases (in memory and larger than memory against Redis and Kvrocks, scaling with
  cores, high availability, crashes and power cuts, a 10-hour soak); results in `bench/results/cloud-*`.

### Added

- Releases: `.github/workflows/release.yml` publishes a tagged version as `ghcr.io/anlohse/datadam:X.Y.Z`,
  `:X.Y` and `:latest`, and as a GitHub release with `datadam-X.Y.Z-linux-x86_64.tar.gz`,
  `datadam_X.Y.Z_amd64.deb` (a systemd service listening on 127.0.0.1), the SPDX documents of the linked
  libraries, and `SHA256SUMS`; every file and the image have a build provenance attestation.
- `datadam --version` (and `--help`); `INFO server` has `datadam_git_sha1`, the startup log line the commit.
- A storage format recorded in the data directory (`INFO persistence`: `storage_format`): a datadam refuses a
  directory written in a newer format instead of misreading it.
- `HREPLACE key field value [field value ...]`: replaces a hash's whole field set atomically, keeping its TTL.
- `RETURNJSON` on `IDX.QUERY` and `FT.SEARCH`: JSON values returned as JSON text (strings quoted, `null`).
- `promotion-handoff`: `REPLICAOF NO ONE` on a replica asks its primary to hand over, as `FAILOVER` does.
- `page-cache-reads`: a read the OS page cache can serve is served on the I/O thread (`preadv2` with
  `RWF_NOWAIT`), without waking a read-pool thread.
- `maxclients`, `client-query-buffer-limit`, `tcp-keepalive`, `enable-protected-configs`.

### Changed

- `ghcr.io/anlohse/datadam:latest` is now the latest release; the latest commit of `main` is `:edge`.
- `io-threads auto` (the default): one I/O thread per CPU up to 8, then half of the CPUs.
- The object cache's shard locks spin before sleeping: 1.10M → 1.47M pipelined mixed operations/s on 16 vCPUs.
- Lua globals are read-only; `CLIENT KILL` and `CLIENT LIST` need `@admin`; `BACKUP` writes only inside
  `backup-dir` unless `enable-protected-configs` allows it.

### Fixed

- A write refused by a `UNIQUE` index inside a transaction or script no longer deletes a key it had not
  touched when the transaction is undone.
- Requests that could crash the server or exhaust its memory: deeply nested JSONPath filters, `FT.SEARCH`
  queries and JSON documents; `..` expansions over deep documents.
- A replication-only user could make the primary step down; replication acknowledgments beyond what was sent
  were trusted.
- `CONFIG REWRITE` lines could be injected through a configuration value.
