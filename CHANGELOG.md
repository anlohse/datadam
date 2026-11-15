# Changelog

What changed in each release of datadam. The format follows [Keep a Changelog](https://keepachangelog.com),
the versions [Semantic Versioning](https://semver.org); what a version promises, and how to upgrade, is in
[docs/releases.md](docs/releases.md).

## [Unreleased]

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
