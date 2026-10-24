# Building and testing

How to build datadam on Windows, Linux and in Docker, what each test suite covers and how to run one test, the
compiler warning settings, the benchmark scripts, and some debugging tips. User-level installation is in
[getting-started.md](../getting-started.md).

## Contents

- [Toolchain](#toolchain)
- [vcpkg and the CMake presets](#vcpkg-and-the-cmake-presets)
- [Building on Windows](#building-on-windows)
- [Building on Linux](#building-on-linux)
- [Docker](#docker)
- [Compiler flags and warnings](#compiler-flags-and-warnings)
- [Unit tests](#unit-tests)
- [Running a single test](#running-a-single-test)
- [End-to-end tests](#end-to-end-tests)
- [Continuous integration](#continuous-integration)
- [Benchmarks](#benchmarks)
- [Debugging tips](#debugging-tips)

## Toolchain

- CMake 3.25 or newer (`cmake_minimum_required(VERSION 3.25)`; the presets file is version 6).
- A C++20 compiler with coroutines and `std::format`: MSVC from Visual Studio 2022 or newer, GCC 13 or newer,
  Clang 17 or newer (the README's list). The Docker build uses GCC 13 from Ubuntu 24.04.
- Ninja on Windows (the `windows` preset uses the Ninja Multi-Config generator); Make on Linux.
- [vcpkg](https://vcpkg.io), found through the `VCPKG_ROOT` environment variable.
- For the scripts: Python 3 (standard library only), a POSIX shell, Bash and Docker for the redis-cli suite.

## vcpkg and the CMake presets

[vcpkg.json](../../vcpkg.json) is a manifest: CMake's vcpkg toolchain installs the dependencies at configure
time.

| Dependency | Used for |
|---|---|
| `asio` | Networking and coroutines (standalone, `ASIO_STANDALONE`, `ASIO_NO_DEPRECATED`) |
| `rocksdb` (without default features, with `lz4` and `zstd`) | Storage; LZ4 for most levels, ZSTD for the bottommost |
| `openssl` | TLS, and generating certificates in `tls_test.cpp` |
| `nlohmann-json` | The JSON type (`nlohmann::ordered_json`) |
| `luajit` | Lua scripting (Lua 5.1, run by the interpreter: the JIT is turned off). Found by `find_path`/`find_library` in CMakeLists.txt, release and debug libraries apart |
| `gtest` | Unit tests |

The `builtin-baseline` pins the vcpkg registry commit; the Dockerfile checks out vcpkg at the same commit
(`VCPKG_COMMIT`).

[CMakePresets.json](../../CMakePresets.json):

| Preset | Kind | Details |
|---|---|---|
| `base` (hidden) | configure | `binaryDir` `build/<preset>`, toolchain `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`, `VCPKG_INSTALLED_DIR` `build/vcpkg_installed` (shared by all presets) |
| `windows` | configure | Ninja Multi-Config, `cl`, triplet `x64-windows`; only on a Windows host |
| `linux` | configure | Unix Makefiles, triplet `x64-linux`, `CMAKE_BUILD_TYPE=Release`; only on a Linux host |
| `windows-release`, `windows-debug` | build | The `Release` / `Debug` configuration of `windows` |
| `linux` | build | The `linux` configure preset |
| `windows-release`, `linux` | test | `ctest` with `outputOnFailure` |

[CMakeLists.txt](../../CMakeLists.txt) defines three targets: `datadam_core` (a static library with every source
file but `main.cpp`), `datadam` (the server) and, with `DATADAM_BUILD_TESTS` (on by default), `datadam_tests`.
New source files must be added to the `datadam_core` list, and new test files to `datadam_tests`.

## Building on Windows

From a "x64 Native Tools Command Prompt" (or after running `vcvars64.bat` of your Visual Studio installation), with
`cl` and `ninja` on the `PATH`:

```bat
set VCPKG_ROOT=D:\cpp\vcpkg
cmake --preset windows
cmake --build --preset windows-release
ctest --preset windows-release
```

- If `ninja` is not on the `PATH`, the copy vcpkg downloads under `%VCPKG_ROOT%\downloads\tools\ninja-*` works.
- The first configure builds every dependency, RocksDB included, which takes a while; later configures reuse
  `build/vcpkg_installed`.
- Outputs: `build\windows\Release\datadam.exe` and `build\windows\Release\datadam_tests.exe` (or `Debug\` with
  `--preset windows-debug`). There is no Debug test preset; use `ctest --test-dir build/windows -C Debug`.
- Opening the folder in Visual Studio also picks up the presets.

Run the server with the sample configuration:

```bat
build\windows\Release\datadam datadam.conf --port 6380 --cache-max-memory 1gb
```

## Building on Linux

```bash
export VCPKG_ROOT=~/vcpkg
cmake --preset linux && cmake --build --preset linux -j && ctest --preset linux
```

Outputs: `build/linux/datadam` and `build/linux/datadam_tests` (Release).

## Docker

The [Dockerfile](../../Dockerfile) is a multi-stage Linux build:

| Stage | What it does |
|---|---|
| `toolchain` | Ubuntu 24.04 with build-essential, CMake, Ninja, git, Python 3; trusts the optional `extra_ca` secret; checks out vcpkg at `VCPKG_COMMIT` and bootstraps it |
| `deps` | Copies only `vcpkg.json` and runs `vcpkg install --triplet x64-linux` into `/src/build/vcpkg_installed`, with a cache mount on `/root/.cache/vcpkg`. This layer is rebuilt only when the manifest changes. `BUILD_JOBS` (default 8) caps the parallelism (`VCPKG_MAX_CONCURRENCY`), since RocksDB translation units need a lot of memory |
| `build` | Copies the sources and runs `cmake --preset linux` and `cmake --build --preset linux` |
| `test` | `ctest --preset linux` and `sh tests/run_integration.sh build/linux/datadam` |
| `runtime` | A clean Ubuntu 24.04 with only the binary and `datadam.conf`; runs as the system user `datadam`, data in the volume `/var/lib/datadam`, owns `/etc/datadam` so `CONFIG REWRITE` works; exposes 6379 and 11211 |

```bash
docker build --target test -t datadam:test .   # build and run unit + integration tests
docker build -t datadam .                      # runtime image
docker run -d -p 6379:6379 -p 11211:11211 -v datadam-data:/var/lib/datadam datadam
```

The entry point is `datadam /etc/datadam/datadam.conf --dir /var/lib/datadam`; arguments given to `docker run`
are appended, so `docker run datadam --wal-retention 1h` works.

**HTTPS interception.** When a proxy or antivirus intercepts HTTPS, the vcpkg downloads fail in the container.
Pass the interceptor's root certificate as a build secret:

```bash
docker build --secret id=extra_ca,src=my-root-ca.crt -t datadam .
```

It is installed with `update-ca-certificates` in the build stages only, never in the runtime image.
[.dockerignore](../../.dockerignore) keeps `build/`, `data/`, IDE folders and logs out of the context.

## Compiler flags and warnings

From [CMakeLists.txt](../../CMakeLists.txt), applied to `datadam_core` and inherited by everything that links it:

| Compiler | Flags |
|---|---|
| MSVC | `/W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj /EHsc /wd4324` (C4324, padding due to `alignas`, is intended: cache-line separation of lock stripes and the like) |
| GCC / Clang | `-Wall -Wextra -Wpedantic` |

Warnings are not errors (no `/WX` or `-Werror`); keep new code free of them on both compilers. Also defined:
`DATADAM_VERSION` (from the project version), `ASIO_STANDALONE`, `ASIO_NO_DEPRECATED`, and on Windows
`_WIN32_WINNT=0x0A00`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`. `CMAKE_EXPORT_COMPILE_COMMANDS` is on, for clangd and
similar tools.

## Unit tests

All suites are GoogleTest files linked into `datadam_tests` with `GTest::gtest_main`; `gtest_discover_tests`
(with `DISCOVERY_MODE PRE_TEST`) lists them when `ctest` runs. Tests that need a database create a fresh
directory under `std::filesystem::temp_directory_path()` with a random suffix, open `RocksStorage` with
`FsyncPolicy::No`, a 4 MiB `ObjectCache` with 2 shards, a `Database` and usually a `ServerContext`, and remove
the directory in `TearDown`. Most command tests then call `execute_redis_command` directly and parse the RESP2
reply, without any socket.

| File | Fixture / suites | What it covers |
|---|---|---|
| [strings_test.cpp](../../tests/strings_test.cpp) | `Strings`, `LatencyHistogram` | `parse_size`, `parse_duration`, number parsers, `glob_match`; histogram percentiles within a bucket |
| [slab_allocator_test.cpp](../../tests/slab_allocator_test.cpp) | `SizeClasses`, `SlabAllocator`, `PageArena` | Geometric bounded classes, distinct chunks, empty pages released, chunk reuse, budget, minimum committed pages |
| [object_cache_test.cpp](../../tests/object_cache_test.cpp) | `ObjectCache` | Insert/lookup/erase, large objects, LRU under pressure, page steal across classes, idle floor, max age, TTL, disabled cache, admission on second access |
| [element_set_test.cpp](../../tests/element_set_test.cpp) | `ElementSetTest` | `ElementSet` against a `std::map` through loads and changes; `append` |
| [resp_test.cpp](../../tests/resp_test.cpp) | `RespParser`, `Reply` | Multibulk, incomplete input at every byte, binary values, inline, protocol errors; RESP2 vs RESP3 replies |
| [database_test.cpp](../../tests/database_test.cpp) | `DatabaseTest` | `Database` API: persistence across restart, cache after first load, `update`, CAS, expiry, `mset`/`rename`/flush, write batches (own reads, lock held until commit, no locks held during disk reads, last write wins, concurrent batches atomic and deadlock-free), `load_many_into_cache` |
| [collections_test.cpp](../../tests/collections_test.cpp) | `CollectionsTest` | All collection types across restart, versions never reused, compaction of orphaned and expired elements, `RENAME`, pipeline batches, failed transactions, zset score order, concurrent list pushes, cached small collections vs RocksDB, large collections not cached whole |
| [index_test.cpp](../../tests/index_test.cpp) | `IndexTest` | `IDX.*`: equality and range queries, maintenance, backfill and restart, UNIQUE (rejection, rollback, backfill failure, concurrent claims), drop, stale entry repair and compaction, pipelined writes, exact counts under concurrency |
| [json_test.cpp](../../tests/json_test.cpp) | `JsonPath`, `Json`, `JsonCommands`, `JsonIndexDef`, `JsonIndexes` | Path selectors and filters, legacy paths, depth limit, every `JSON.*` command family, JSON index definitions, queries and UNIQUE |
| [search_test.cpp](../../tests/search_test.cpp) | `SearchIndexDef`, `SearchQuery`, `Search` | Search entry encoding, compound tuples, the query parser, `FT.SEARCH` over hashes and JSON, backfill, multi-valued fields |
| [ops_test.cpp](../../tests/ops_test.cpp) | `Ops`, `LiveOps` | Uncommitted writes invisible to other clients (also inside `EXEC`), backups opened as databases, `BGSAVE` retention, `SLOWLOG` truncation and redaction, `OBJECT`/`TOUCH`/`MEMORY USAGE`; on a real server `CLIENT LIST`/`KILL` (including a blocked client), `MONITOR`, `SHUTDOWN` |
| [realtime_test.cpp](../../tests/realtime_test.cpp) | `Commands`, `Live` | `MULTI`/`EXEC`/`WATCH` semantics and isolation between blocks, Pub/Sub replies, patterns, shard channels, RESP3 pushes and channel ACLs; on a real server: `BLPOP`/`BLMOVE`/`BZPOPMIN`/`BLMPOP` waits and timeouts, one element per waiter, a client that leaves while blocked, message delivery from several publishers |
| [admin_test.cpp](../../tests/admin_test.cpp) | `AdminTest` | SCAN family (exactly once, despite writes, cursors), CONFIG SET all-or-nothing, AUTH, memcache auth, deferred reads, read pool single flight and batching |
| [acl_test.cpp](../../tests/acl_test.cpp) | `AclTest` | Command and key permissions, read-only/write-only patterns, subcommands and categories, filtered KEYS/SCAN/IDX, changes reaching open connections, memcache permissions, CONFIG REWRITE, ACL LOG |
| [cdc_test.cpp](../../tests/cdc_test.cpp) | `CdcTest` | `CDC.READ` decoding of strings, collections and JSON, cursors, COUNT, MATCH, `$`, ACL filtering, BLOCK, restart, WAL size limit |
| [replication_test.cpp](../../tests/replication_test.cpp) | `ReplicationTest` | Two real servers in one process: full sync then stream, restarted replica continues, promotion, primary without kept WAL, replica behind the kept WAL, `WAIT`, `FAILOVER`, function libraries following the primary |
| [script_test.cpp](../../tests/script_test.cpp) | `Scripts`, `LiveScripts` | Lua reply conversion both ways, `redis.call`/`pcall` and their errors, the script cache, the sandbox, `cjson`, read-only scripts, per-command ACL checks, scripts in `EXEC`, `setresp`, functions (load, replace, list, delete, kept across restarts); on a real server: atomic read-modify-write under concurrency, `SCRIPT KILL`, `UNKILLABLE`, commands waiting for a script |
| [tls_test.cpp](../../tests/tls_test.cpp) | `TlsTest` | A real server with TLS and plain ports, certificates generated with the OpenSSL API: CN login, refused certificates, reload with CONFIG SET, ACL LOG |

`ReplicationTest` and `TlsTest` start real `Server` objects with `Server::start(true)` (every listener on an
ephemeral port) and run them on a thread.

**TLS interception.** Some antivirus products intercept TLS even to 127.0.0.1. In `TlsTest::SetUp`, the test
connects once and reads the issuer of the certificate it is presented; if it is not the test CA (`/CN=test-ca`),
every TLS test is skipped with `GTEST_SKIP` and a message naming the issuer. They run normally in Docker.

## Running a single test

```bash
# by ctest name (the gtest Suite.Test names), regular expression
ctest --preset windows-release -R "CdcTest\."
ctest --preset linux -R DatabaseTest.BatchHoldsKeyUntilCommit --output-on-failure

# or the test binary directly, with gtest flags
build/windows/Release/datadam_tests --gtest_filter=AdminTest.ReadPool*
build/linux/datadam_tests --gtest_filter='CollectionsTest.*' --gtest_repeat=20
```

`--gtest_repeat` together with a filter is the quickest way to shake out a flaky concurrency test.

## End-to-end tests

**[tests/run_integration.sh](../../tests/run_integration.sh)** `[path/to/datadam]` (default `build/linux/datadam`;
env `PORT`, default 6390, and `MC_PORT`, default 11290) starts the server with `datadam.conf` on a temporary data
directory, runs [integration_test.py](../../tests/integration_test.py), kills the server with `kill -9`, restarts
it and runs the `verify` phase, then sends SIGTERM and waits for a clean exit. The Docker `test` stage runs it.

**[tests/integration_test.py](../../tests/integration_test.py)** `[--host H] [--port 6379] [--memcache-port 11211]
[--phase all|write|verify]` checks RESP and memcache end to end against any running server, with the standard
library only; `write` stores a marker and `verify` checks it after a restart.

**[tests/redis_cli_test.sh](../../tests/redis_cli_test.sh)** `[host] [port] [phase]` drives the official
`redis-cli`:

- `commands` (default): every supported command, edge cases and error replies, in sections (connection, strings,
  counters, keyspace, cache, JSON, hash, set, sorted set, list, indexes, search, scan, CONFIG SET, AUTH, ACL,
  change feed);
- `write`: stores a known data set (keys, TTLs, users, an index, a rewritten config);
- `verify`: checks that data set, to run after a restart.

The helpers compare `redis-cli` output: `t` (exact), `tc` (substring), `tx` (regex), `ts` (sorted), `tp` / `tpc`
(several commands on one connection), `tu` (as another user). `CLI_OPTS` adds options to every `redis-cli` call
(for TLS). The script prints `ok` / `FAIL` per check and exits non-zero if anything failed.

**[tests/docker_sentinel_test.sh](../../tests/docker_sentinel_test.sh)** (image `datadam:latest`, or `IMAGE=...`)
starts a primary, a replica and three `redis-sentinel` containers from `redis:7.4`, stops the primary, and checks
that the sentinels promote the replica, that it takes writes, and that the old primary, started again, becomes
its replica.

**[tests/docker_redis_cli.sh](../../tests/docker_redis_cli.sh)** `[image]` (default `datadam:latest`) runs the
image on a private Docker network and tests it with `redis-cli` from the `redis:7.4` image:

1. the server with `--wal-retention 1h`, and the `commands` phase;
2. two persistence cycles: `write`, `docker stop` (SIGTERM) or `docker kill -s KILL`, a 3-second pause (so a
   1.5-second TTL key expires while the server is down), restart, `verify`;
3. TLS: generates a CA, a server and a client certificate with `openssl`, starts a second container with
   `--tls-port 6380 ... --tls-auth-clients-user cn`, runs the whole `commands` phase over TLS (`CLI_OPTS`), and
   checks refusals, CN login and ACL LOG;
4. replication: a third container with `--replicaof` the first, checking full sync, streamed writes,
   `READONLY`, `ROLE`, `INFO replication` and promotion with `REPLICAOF NO ONE`.

It needs Docker, `openssl` on the host and the image built first (`docker build -t datadam .`). Under Git Bash
on Windows it sets `MSYS_NO_PATHCONV=1` so volume paths are not rewritten.

## Continuous integration

[.github/workflows/ci.yml](../../.github/workflows/ci.yml) runs on every push to `main`, on pull requests and
by hand (`workflow_dispatch`); a newer push to the same branch cancels the run in progress.

| Job | Runner | What it does |
|---|---|---|
| Linux | `ubuntu-24.04` | Builds the Dockerfile's `deps` stage (cached in the GitHub Actions cache, scope `linux-deps`), then its `test` stage (build, `ctest`, `run_integration.sh`), then the runtime image; runs `tests/docker_redis_cli.sh` and `tests/docker_sentinel_test.sh` against it. |
| Windows | `windows-2022` | Fetches vcpkg at the manifest's baseline, configures with the `windows` preset in a VS developer environment (`ilammy/msvc-dev-cmd`), builds `windows-release` and runs `ctest`. |

The first run builds every dependency (RocksDB is the slow one) and takes much longer than the next ones. On
Windows the dependencies are built for the `x64-windows-release` triplet (CI builds Release only), and the
cache key includes the runner image version (`ImageVersion`): vcpkg's package hashes include the compiler's,
which changes with the image, so each new image costs one rebuild. The cache is keyed by the vcpkg commit and
the hash of `vcpkg.json` too, and is saved right after the dependencies are
built, so a failing build or test does not throw it away. On Linux, the `deps` layer is built and exported in
a step of its own for the same reason. Changing `vcpkg.json` (or the baseline, which must move in
`vcpkg.json`, the Dockerfile's `VCPKG_COMMIT` and the workflow's `VCPKG_COMMIT` together) rebuilds them.

The TLS tests, which skip themselves on machines whose antivirus intercepts TLS, run on both runners.

## Benchmarks

The scripts in [bench/](../../bench/) compare datadam with Redis and Kvrocks using memtier_benchmark, each server
alone in a container with a hard memory limit. Results and methodology are in
[bench/RESULTADOS.md](../../bench/RESULTADOS.md) (Portuguese) and summarized in [benchmarks.md](../benchmarks.md).

| Script | Usage |
|---|---|
| [memtier.sh](../../bench/memtier.sh) | `bench/memtier.sh <fit\|big> [server ...]` (servers: `datadam redis kvrocks`). `fit`: 1M keys of 512 B in 2 GB containers; `big`: 6M keys in 1 GB containers. Env: `KEYS`, `DATA`, `MEM`, `TIME`, `SEEDS` (`same`, memtier's default, or `distinct`). Results go to `bench/results/<scenario>-<time>/` |
| [load_random.py](../../bench/load_random.py) | `python load_random.py <host> <port> <keys> <value-bytes> [processes]`: loads incompressible random values (memtier's own random data compresses almost entirely) |
| [report.py](../../bench/report.py) | `python bench/report.py bench/results/<dir>`: tables from a results directory |
| [repro_tail.sh](../../bench/repro_tail.sh) | `bench/repro_tail.sh [image] [workload args...]`: one workload against one image, then what the server saw (batch conflicts, cache, RocksDB stalls) |

Things learned the hard way: use `SEEDS=distinct` (with the default, all clients ask for the same keys in
lockstep); only compare servers measured in the same run, since Docker Desktop's disk is cached by the host and
varies a lot between runs; ignore memtier's reported maximum latency and use percentiles; and diagnose on the
server side with `CONFIG RESETSTAT` before a run and `INFO latency` after it.

## Debugging tips

- **Logs.** `--log-level debug` (or `CONFIG SET log-level debug`) logs connections, disconnections with the
  error that ended them, TLS handshake failures and cache maintenance evictions. Logs go to stderr.
- **Where the time goes.** `CONFIG RESETSTAT`, run the workload, then `INFO latency`: RocksDB get/write times,
  key lock waits, how long batches hold their locks, read pool latency. `INFO persistence` shows
  `write_batch_conflicts`; `INFO stats` the deferred reads and read pool counters; `CACHE STATS` and
  `CACHE CLASSES` the cache.
- **Isolate a subsystem** with settings:
  - `--read-threads 0`: cache misses are read on the I/O thread (no deferral, no read pool);
  - `CONFIG SET write-batching no`: each command commits its own write;
  - `--cache-max-memory 0 --cache-min-memory 0`: no object cache, every read goes to RocksDB (`validate`
    rejects a `cache-min-memory` above `cache-max-memory`); `CACHE CLEAR` empties the cache at run time;
  - `CONFIG SET cache-admission no`, `CONFIG SET cache-on-write no`;
  - `--experimental-write-path wal-buffered|no-wal|memory`: measures the write path without the WAL (never for
    real data).
- **In a debugger**, the I/O threads are all in `asio::io_context::run`; a connection's state lives in the
  coroutine frame of `Server::serve_redis` (its `ClientState`, `in`, `out`). Set `--io-threads 1` and
  `--read-threads 0` to make stepping through a request deterministic.
- **Inspect a data directory** with a RocksDB tool such as `ldb` (not built by this project), with the server
  stopped: the column families are `meta`, `data`, `index` and `default`, and the key and value layouts are in
  [storage-format.md](storage-format.md).
- **Sanitizers** are not set up in `CMakeLists.txt` or the presets; pass the flags yourself (for example
  `-DCMAKE_CXX_FLAGS=-fsanitize=address` with GCC or Clang) in a separate build directory.
