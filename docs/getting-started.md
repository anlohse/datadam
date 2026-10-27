# Getting started

This page builds datadam, starts it, and walks through a first session over the Redis and memcache protocols.
It ends with where the data lives on disk and how to stop the server without losing anything.

- [What datadam is](#what-datadam-is)
- [Building](#building)
- [Running](#running)
- [A first session](#a-first-session)
- [Where the data lives](#where-the-data-lives)
- [Stopping cleanly](#stopping-cleanly)
- [Next steps](#next-steps)

## What datadam is

datadam is a persistent key-value store that speaks the Redis protocol (RESP2 and RESP3) and the memcache text
protocol. Every write goes to RocksDB, which is the source of truth on disk. In front of RocksDB sits an object
cache on a memory pool of its own, so hot keys are served from memory. On top of strings, datadam offers
hashes, sets, sorted sets and lists, a JSON type compatible with RedisJSON, secondary indexes (`IDX.*`),
RediSearch-compatible search (`FT.*`) over `TAG` and `NUMERIC` fields, a change feed (`CDC.*`) and primary to
replica replication. Keys are shared between the two protocols: a value written with memcache `set` can be
read with Redis `GET`, and the reverse.

There is a single database (`SELECT 0` only). Data is never evicted: the cache only decides what stays in
memory, and everything is on disk.

## Building

Requirements:

- CMake 3.25 or later.
- A C++20 compiler: MSVC 2022 or later, GCC 13 or later, or Clang 17 or later.
- [vcpkg](https://vcpkg.io), with the `VCPKG_ROOT` environment variable pointing at it.

The dependencies come from `vcpkg.json` and are built by vcpkg on the first configure: Asio, RocksDB (with the
`lz4` and `zstd` features), OpenSSL, nlohmann-json, LuaJIT and GoogleTest. The first build takes a while, mostly for
RocksDB.

`CMakePresets.json` defines one configure preset per platform. Both put the build tree in `build/<preset>` and
the vcpkg packages in `build/vcpkg_installed`. Unit tests are built by default; pass
`-DDATADAM_BUILD_TESTS=OFF` to the configure step to skip them.

### Windows

Use an "x64 Native Tools Command Prompt for VS 2022" (or open the folder in Visual Studio), so that `cl` is on
the `PATH`. The `windows` preset uses the Ninja Multi-Config generator, so `ninja` must be on the `PATH` too.

```bat
set VCPKG_ROOT=D:\path\to\vcpkg
cmake --preset windows
cmake --build --preset windows-release
ctest --preset windows-release
```

The binary is `build\windows\Release\datadam.exe`. A debug build uses the `windows-debug` build preset and
lands in `build\windows\Debug\`.

### Linux

```bash
export VCPKG_ROOT=~/vcpkg
cmake --preset linux
cmake --build --preset linux -j
ctest --preset linux
```

The `linux` preset is a Release build with Unix Makefiles. The binary is `build/linux/datadam`.

`tests/run_integration.sh build/linux/datadam` starts a fresh server, runs `tests/integration_test.py`
against it, kills it with `kill -9`, restarts it, checks that the data survived, and finally checks that
`SIGTERM` shuts it down cleanly. See [Building and testing](dev/building-and-testing.md) for the whole test
setup.

### Docker

The `Dockerfile` builds on Ubuntu 24.04 with GCC 13, using the same vcpkg commit as the `builtin-baseline` of
`vcpkg.json`. The dependencies get a layer of their own, rebuilt only when `vcpkg.json` changes.

```bash
docker build --target test -t datadam:test .   # builds, then runs the unit and integration tests
docker build -t datadam .                      # slim runtime image with only the binary
```

Build arguments:

| Argument | Default | Meaning |
|---|---|---|
| `BUILD_JOBS` | `8` | Parallel compile jobs (RocksDB translation units are memory hungry; lower it on small Docker VMs). |
| `VCPKG_COMMIT` | the manifest's baseline | vcpkg commit to check out. |

**Networks that intercept HTTPS.** When a proxy or an antivirus (Avast, for example) intercepts HTTPS, vcpkg
cannot download the dependencies inside the build. Pass the interceptor's root certificate as the `extra_ca`
build secret:

```bash
docker build --secret id=extra_ca,src=my-root-ca.crt -t datadam .
```

The certificate is trusted only in the build stages; it never reaches the runtime image.

The runtime image:

- installs the binary as `/usr/local/bin/datadam` and the sample `datadam.conf` as
  `/etc/datadam/datadam.conf`;
- runs as the unprivileged system user `datadam`, which owns `/etc/datadam` (so `CONFIG REWRITE` can write the
  file) and `/var/lib/datadam`;
- declares `/var/lib/datadam` as a volume and exposes ports 6379 and 11211;
- has the entry point `datadam /etc/datadam/datadam.conf --dir /var/lib/datadam`, so arguments given to
  `docker run` after the image name are appended as further `--option value` overrides;
- turns [protected mode](security.md#protected-mode) off, as the Redis image does: set `requirepass` before
  publishing a port on an untrusted network.

CI publishes the image of every commit on `main` that passes all the tests to the GitHub Container Registry,
as `ghcr.io/anlohse/datadam:latest` and `ghcr.io/anlohse/datadam:sha-<commit>`:

```bash
docker run -d --name datadam -p 6379:6379 -v datadam-data:/var/lib/datadam ghcr.io/anlohse/datadam:latest \
  --requirepass s3cret
```

```bash
docker run -d --name datadam -p 6379:6379 -p 11211:11211 -v datadam-data:/var/lib/datadam datadam
docker run -d --name datadam -p 6379:6379 -v datadam-data:/var/lib/datadam datadam \
  --cache-max-memory 1gb --memcache-port 0
```

See [Operations](operations.md#docker-deployment) for volumes, memory limits and keeping a rewritten config
file across container re-creation.

## Running

```text
datadam [config-file] [--option value]...
```

The configuration file is optional and, when given, must be the first argument. Every option of the file can
also be given on the command line as `--option value`; command-line values are applied after the file, so they
win. Without a file, every option not given on the command line keeps its built-in default.

```bash
build/linux/datadam datadam.conf
build/linux/datadam datadam.conf --port 6380 --cache-max-memory 1gb
build/linux/datadam --dir /srv/datadam --requirepass change-me
```

The sample `datadam.conf` at the root of the repository lists every common option with a comment.
[Configuration](configuration.md) describes all of them, the size and duration syntax, and which ones
`CONFIG SET` can change while the server runs.

A configuration error stops the server before it opens anything, with a message and the usage line on
standard error and exit status 1:

```text
datadam: datadam.conf:12: invalid value 'sometimes' for 'fsync'
usage: datadam [config-file] [--option value]...
  e.g. datadam datadam.conf --port 6380 --cache-max-memory 1gb
```

On a successful start the server logs (to standard error) what it listens on and how the cache is set up:

```text
2026-10-23 09:15:02.418 INFO  datadam 0.1.0 | redis 0.0.0.0:6379 | tls off | memcache 11211 | io threads 8 | data dir ./data | fsync everysec
2026-10-23 09:15:02.418 INFO  cache: max 512mb min 64mb objects <= 4kb | page 64kb | 18 size classes | idle 300000ms age 0ms
```

Log timestamps are UTC. The exact number of size classes depends on `cache-max-object-size`.

When the server is reachable from other machines, set `requirepass` (or define ACL users) and consider TLS;
see [Security](security.md).

## A first session

With the server running on the default ports, use the standard `redis-cli`:

```text
$ redis-cli -p 6379
127.0.0.1:6379> PING
PONG
127.0.0.1:6379> SET greeting hello EX 3600
OK
127.0.0.1:6379> GET greeting
"hello"
127.0.0.1:6379> TTL greeting
(integer) 3600
127.0.0.1:6379> HSET user:1 name ana city SP
(integer) 2
127.0.0.1:6379> HGETALL user:1
1) "name"
2) "ana"
3) "city"
4) "SP"
127.0.0.1:6379> INFO server
# Server
datadam_version:0.1.0
redis_version:7.2.0
redis_mode:standalone
...
```

datadam reports Redis version 7.2.0 (in `INFO` and `HELLO`) for clients that gate features on it. `HELLO 3`
(or `redis-cli -3`) switches the connection to RESP3.

The same keys are visible over memcache. With `nc` (some variants need `-q 1` or `-N` to exit after the reply):

```bash
$ printf 'get greeting\r\n' | nc localhost 11211
VALUE greeting 0 5
hello
END
$ printf 'set counter 0 0 1\r\n5\r\n' | nc localhost 11211
STORED
$ redis-cli INCR counter
(integer) 6
```

When `requirepass` is set, Redis clients must authenticate first (`AUTH <password>`, `redis-cli -a`, or the
`REDISCLI_AUTH` environment variable), and a memcache connection must first send `auth <password>` (or
`auth <user> <password>`), because the memcache text protocol has no authentication of its own.

To try a secondary index:

```text
127.0.0.1:6379> IDX.CREATE users PREFIX 1 user: SCHEMA city TAG
OK
127.0.0.1:6379> IDX.QUERY users city = SP
...
```

See [Commands](commands.md), [Secondary indexes](secondary-indexes.md), [Search](search.md) and
[JSON](json.md) for what is supported, and [memcache](memcache.md) for the memcache side.

## Where the data lives

Everything is stored in the RocksDB database in the `dir` directory (`./data` by default, relative to the
working directory of the process; the Docker image uses `/var/lib/datadam`). The directory is created if it
does not exist. It holds the usual RocksDB files:

| Files | Content |
|---|---|
| `*.sst` | Sorted, compressed data files (LZ4, ZSTD on the bottom level). |
| `*.log` | The write-ahead log (WAL): writes not yet flushed into SST files. |
| `archive/` | WAL files kept after RocksDB no longer needs them, only when `wal-retention` is set (change feed, replication). |
| `CURRENT`, `MANIFEST-*`, `OPTIONS-*` | RocksDB metadata. |
| `LOCK` | Held by the running process: a second server cannot open the same directory. |
| `LOG`, `LOG.old.*` | RocksDB's own diagnostic log (the last 5 are kept). |

The data is split into four column families: `meta` (one record per key), `data` (elements of collections),
`index` (secondary index entries) and the default one (index definitions and other server state). The
layout is described in [Storage format](dev/storage-format.md).

The object cache is memory only; it is rebuilt from disk as keys are read after a restart.

## Stopping cleanly

Send `SIGINT` (Ctrl+C in a terminal, also on Windows) or `SIGTERM` (`kill <pid>`, `docker stop`). The server
logs `signal N received, shutting down`, stops accepting work, syncs the WAL to disk, closes RocksDB and logs
`stopped`. There is no `SHUTDOWN` command.

A crash or `kill -9` does not lose acknowledged writes in the default configuration: every write reaches the
WAL (a `write` system call) before its reply is sent, and RocksDB replays the WAL on the next start. With the
default `fsync everysec`, a power loss can lose up to about the last second. See
[Operations](operations.md#durability) for the details and the other modes.

## Next steps

- [Configuration](configuration.md): every option, units, `CONFIG GET/SET/REWRITE`.
- [Operations](operations.md): durability, memory sizing, monitoring with `INFO`, backups, Docker,
  troubleshooting.
- [Commands](commands.md): the supported Redis commands.
- [Security](security.md): passwords, ACL users, TLS.
- [Replication](replication.md) and [Change feed](change-feed.md).
- [Benchmarks](benchmarks.md): measured results against Redis and Kvrocks.
- [Architecture](dev/architecture.md), for how it works inside.
