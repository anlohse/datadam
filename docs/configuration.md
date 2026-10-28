# Configuration

datadam reads its settings from an optional configuration file and from `--option value` arguments on the
command line. This page lists every option the server accepts, the value syntax, which options `CONFIG SET`
can change while the server runs, and how `CONFIG GET`, `CONFIG SET`, `CONFIG REWRITE` and `CONFIG RESETSTAT`
behave.

- [Where settings come from](#where-settings-come-from)
- [Value syntax](#value-syntax)
- [Options](#options)
  - [Network](#network)
  - [Persistence and RocksDB](#persistence-and-rocksdb)
  - [Change feed](#change-feed)
  - [Object cache and memory pool](#object-cache-and-memory-pool)
  - [Read pool](#read-pool)
  - [Logging](#logging)
  - [Replication](#replication)
  - [Security and ACL](#security-and-acl)
  - [TLS](#tls)
  - [Experimental](#experimental)
- [Startup checks](#startup-checks)
- [CONFIG GET](#config-get)
- [CONFIG SET](#config-set)
- [CONFIG REWRITE](#config-rewrite)
- [CONFIG RESETSTAT](#config-resetstat)
- [Annotated example](#annotated-example)

## Where settings come from

```text
datadam [config-file] [--option value]...
```

1. Every option starts at its built-in default (the "Default" column below).
2. If the first argument does not start with `--`, it is the configuration file, read line by line.
3. Each `--option value` pair on the command line is then applied in order, so it overrides the file.
4. The result is checked as a whole (see [Startup checks](#startup-checks)).

Any error stops the server with exit status 1, a message on standard error and the usage line. Messages look
like `datadam: datadam.conf:7: invalid value 'x' for 'port'`, `datadam: unknown config option 'prot'`,
`datadam: missing value for '--port'` or `datadam: unexpected argument 'foo'`.

### File syntax

- One setting per line: the option name, whitespace, the value. Option names are case-insensitive.
- A line whose first non-blank character is `#` is a comment. Blank lines are ignored. Windows line endings
  are accepted.
- A setting line must have exactly two words. There are no inline comments (`port 6379 # main` is an error,
  `expected 'key value'`) and no quoting, so a value cannot contain spaces. Lists use commas
  (`tls-protocols TLSv1.2,TLSv1.3`).
- Two exceptions take more words: `replicaof <host> <port>` (or `slaveof`) and `user <name> <rule>...`.
- When an option appears twice, the later line wins. `user` lines accumulate: each one defines a user.

### Command-line syntax

Each option takes exactly one following argument. Quote values that contain spaces:

```bash
datadam datadam.conf --port 6380 --cache-max-memory 1gb
datadam --replicaof "10.0.0.1 6379" --masterauth change-me
datadam --user "app on >app-password ~app:* +@read +@write"
```

`--user` can be repeated, like the `user` lines of the file.

## Value syntax

| Kind | Syntax | Examples |
|---|---|---|
| Size | A whole number, optionally followed by a unit, no space: `b`, `k`/`kb`, `m`/`mb`, `g`/`gb`, `t`/`tb` (powers of 1024, case-insensitive). No unit means bytes. | `4096`, `4kb`, `64mb`, `1gb` |
| Duration | A whole number followed by `ms`, `s`, `m` or `min`, `h`, `d`. No unit means seconds. | `500ms`, `30`, `5m`, `1h` |
| Boolean | `yes`/`no`, `true`/`false`, `on`/`off`, `1`/`0` (case-insensitive). | `yes` |
| Integer | Decimal, within the range given for the option. | `8` |

Fractions (`1.5gb`) and negative values are not accepted. `CONFIG GET` and `CONFIG REWRITE` print sizes and
durations in the largest unit that represents them exactly (`512mb`, `4kb`, `5m`, `1s`, `0`), and booleans
as `yes`/`no`.

## Options

"Runtime" says whether `CONFIG SET` can change the option while the server runs. Every other option is fixed
at startup, and `CONFIG SET` answers `can't set immutable config` for it.

### Network

| Option | Type | Default | Runtime | Description |
|---|---|---|---|---|
| `bind` | IP address | `0.0.0.0` | no | Address every listener binds to (Redis, TLS and memcache). One literal IPv4 or IPv6 address; host names are not resolved. |
| `protected-mode` | boolean | `yes` (`no` in the Docker image) | yes | While the `default` user needs no password, refuse clients that are not on the loopback interface (TLS clients with a verified certificate pass). See [Security](security.md#protected-mode). |
| `port` | 0–65535 | `6379` | no | Plain-text Redis protocol port. `0` disables it, which requires `tls-port`. |
| `memcache-port` | 0–65535 | `11211` | no | memcache text protocol port. `0` disables it. |
| `metrics-port` | 0–65535 | `0` | no | HTTP port for monitoring: `GET /metrics` (Prometheus text format), `/health` and `/ready`. `0` disables it. See [Metrics over HTTP](operations.md#metrics-over-http). |
| `io-threads` | 1–1024 or `auto` | `auto` | no | Threads that run the network event loop and execute commands. `auto`: one per CPU up to 8, then half the CPUs (16 CPUs: 8), which leaves CPU to RocksDB's flushes and compactions and to the read pool; on 16 vCPUs, 8 I/O threads served 20% more of a mixed read/write load than 16 (a read-only load was measured only with 16). `INFO server` shows the number in use. |

`INFO server` shows `tcp_port`, `memcache_port` and `io_threads`.

### Persistence and RocksDB

| Option | Type | Default | Runtime | Description |
|---|---|---|---|---|
| `dir` | path | `./data` | no | RocksDB directory, created if missing. Relative paths are relative to the working directory. |
| `fsync` | `always`, `everysec`, `no` | `everysec` | yes | When the WAL is synced to stable storage. `always`: every write (or write batch) is synced before its reply. `everysec`: a background thread syncs the WAL once per second. `no`: the operating system decides (the WAL is still synced on a clean shutdown). `always` requires `experimental-write-path wal`. |
| `write-batching` | boolean | `yes` | yes | Group commit: the writes of all requests read together from one connection go to RocksDB as one write batch, committed before any of their replies is sent. With `no`, every write is its own RocksDB write. |
| `rocksdb-block-cache` | size | `64mb` | yes | Capacity of RocksDB's LRU block cache, shared by all column families. Index and filter blocks also live in it (in a high-priority half). This is what serves values larger than `cache-max-object-size` and collection elements from memory. |
| `rocksdb-write-buffer` | size | `64mb` | yes | Size of one memtable, per column family. Up to 4 memtables per column family can exist while flushes catch up, in each of the 4 column families. |
| `rocksdb-background-jobs` | 1–256 | `4` | no | RocksDB background flush and compaction threads. |
| `backup-dir` | path | empty | yes | Where `BGSAVE` writes its backups (RocksDB checkpoints), one directory per call named `yyyymmdd-hhmmss` (UTC). Empty: `BGSAVE` only flushes, like `SAVE`. See [Operations](operations.md#backups). |
| `backup-keep` | 0–100000 | `7` | yes | After a `BGSAVE`, only this many of the newest backups in `backup-dir` are kept; `0` keeps them all. |

Every write reaches the WAL before its reply is sent, whatever the `fsync` mode, so a crash of the process
loses nothing; `fsync` only decides what a power loss or kernel crash can lose. See
[Operations](operations.md#durability).

### Change feed

| Option | Type | Default | Runtime | Description |
|---|---|---|---|---|
| `wal-retention` | duration | `0` | no | How long RocksDB keeps WAL files after it no longer needs them for recovery, so `CDC.READ` and replicas can read the changes in them. `0` turns the change feed off. Otherwise at least `1s`; it is rounded down to whole seconds. Needs `experimental-write-path wal` or `wal-buffered`. A primary needs it for replication. |
| `wal-retention-size` | size | `1gb` | no | Upper bound for the kept WAL. Once a second, while the kept files add up to more than this, the oldest are deleted. `0` removes the size bound (only `wal-retention` applies). |
| `wal-retention-hold` | size | `8gb` | no | How far past `wal-retention-size` the kept WAL may grow while a connected replica still needs it: one in a full sync continues from the WAL written meanwhile, which heavy writes can push past `wal-retention-size` (it would then sync again, and again). Past this, the oldest goes anyway. `0` holds nothing. |

See [Change feed](change-feed.md) and [Replication](replication.md).

### Object cache and memory pool

| Option | Type | Default | Runtime | Description |
|---|---|---|---|---|
| `cache-max-memory` | size | `512mb` | no | Hard ceiling of the memory pool. The address range is reserved up front; pages get physical memory only when used. `0` (or less than one pool page) disables the cache, and every read goes to RocksDB. `CONFIG GET maxmemory` reports this value in bytes. |
| `cache-min-memory` | size | `64mb` | yes | Idle eviction (`cache-max-idle`) never shrinks the cache below this. At startup it also sets how many freed pool pages stay committed for reuse instead of going back to the operating system; `CONFIG SET` changes only the idle-eviction floor. Must not exceed `cache-max-memory`. |
| `cache-max-object-size` | size, 64b–1mb | `4kb` | no | Largest object (key plus value) kept in the pool. Larger values are always read from RocksDB (and its block cache). It also sets the pool page size: the larger of 64 KB and four times the largest chunk, rounded up to a power of two. |
| `cache-max-idle` | duration | `5m` | yes | Items not read for this long are released by the maintenance thread, down to `cache-min-memory`. `0` disables idle eviction. |
| `cache-max-age` | duration | `0` | yes | Items loaded longer ago than this are released, even if they are hot. `0` disables it. |
| `cache-maintenance-interval` | duration, at least 10ms | `1s` | no | How often the maintenance thread runs idle, age and TTL eviction. |
| `cache-shards` | 1–1024 | `16` | no | Number of cache shards, each with its own lock, hash table and LRU lists. |
| `cache-on-write` | boolean | `yes` | yes | Write-through: writes put the new value in the cache. With `no`, a write removes the cached copy and only reads populate the cache. |
| `cache-admission` | boolean | `yes` | yes | Admission filter (the TinyLFU doorkeeper): once the pool is full, a key that is not cached gets in only on its second access within a short window, so keys touched once (a bulk load, a scan, the long tail of a large data set) do not push out the hot ones. A key loaded for a client that waits for it is always admitted. With `no`, every access is cached. |

See [Operations](operations.md#memory-and-cache-sizing) for sizing and
[Memory and cache internals](dev/memory-and-cache.md) for how the pool works.

### Read pool

| Option | Type | Default | Runtime | Description |
|---|---|---|---|---|
| `read-threads` | 0–1024 | `64` | no | Threads that load keys missing from the cache for plain Redis reads (`GET`, `MGET`, `EXISTS`, `STRLEN`, `GETRANGE`, `SUBSTR`, `TYPE`, `TTL`, `PTTL`, `EXPIRETIME`, `PEXPIRETIME`, `JSON.GET`, `JSON.MGET`, `JSON.TYPE`, `JSON.STRLEN`, `JSON.ARRLEN`, `JSON.ARRINDEX`, `JSON.OBJKEYS`, `JSON.OBJLEN`), so a disk read never holds an I/O thread. A key wanted by several clients at once is read once. `0` disables the pool: the I/O thread reads the disk itself. |
| `read-batch` | 1–1024 | `1` | no | When more keys are queued than there are read threads, each thread takes up to this many and reads them with one RocksDB `MultiGet`. It saves CPU, but RocksDB reads different SST files one after another; on random keys it measured no faster than `1`. |

See [Operations](operations.md#disk-reads-and-the-read-pool).

### Logging

| Option | Type | Default | Runtime | Description |
|---|---|---|---|---|
| `log-level` | `debug`, `info`, `warn` (or `warning`), `error` | `info` | yes | Minimum level of the server log, written to standard error with a UTC timestamp. |
| `slowlog-log-slower-than` | integer, microseconds | `10000` | yes | `SLOWLOG` records commands that take at least this long. `0`: every command; `-1`: none. |
| `slowlog-max-len` | 0–1000000 | `128` | yes | Entries `SLOWLOG` keeps (the oldest go first). |

RocksDB writes its own diagnostic log to `LOG` in `dir`; `log-level` does not affect it.

### Replication

| Option | Type | Default | Runtime | Description |
|---|---|---|---|---|
| `replicaof` (alias `slaveof`) | `<host> <port>` or `no one` | empty | no (use the `REPLICAOF` command) | Follow this primary as a read-only replica. Empty or `no one`: this server is a primary. Port 1–65535. |
| `masteruser` | text | empty | yes | User name to log in to the primary with. Empty: `AUTH <masterauth>` as the default user. |
| `tls-replication` | boolean | `no` | yes | A replica connects to its primary with TLS; see [Replication](replication.md#replication-over-tls). |
| `tls-client-cert-file`, `tls-client-key-file` | path | empty | yes | The certificate and key a replica presents to its primary over TLS; empty: `tls-cert-file` / `tls-key-file`. |
| `replica-priority` (alias `slave-priority`) | 0–1000000 | `100` | yes | Reported as `slave_priority` in `INFO replication`, for Redis Sentinel: the lowest non-zero priority is promoted first, `0` never. |
| `masterauth` | text, no spaces | empty | yes | Password for the primary. Empty: no `AUTH` is sent. |

A change of `masteruser` or `masterauth` applies the next time the replica connects to its primary. The
`REPLICAOF host port` and `REPLICAOF NO ONE` commands change the role at run time, and `CONFIG REWRITE`
saves the current role as a `replicaof` line. See [Replication](replication.md).

### Security and ACL

| Option | Type | Default | Runtime | Description |
|---|---|---|---|---|
| `requirepass` | text, no spaces | empty | yes | Password of the `default` user. When set, Redis clients must `AUTH` before any other command, and memcache clients must send `auth <password>`. Setting it with `CONFIG SET` replaces the default user's passwords; setting it to an empty string makes the default user password-less again. Connections already authenticated stay authenticated. |
| `user` | `<name> <rule>...` | none | no (use `ACL SETUSER`) | Defines an ACL user from scratch with Redis ACL rules. Repeatable. Applied after `requirepass`, so a `user default ...` line overrides it. |
| `acllog-max-len` | 0–100000 | `128` | yes | Entries kept by `ACL LOG`. |

`CONFIG GET` shows `requirepass` and `masterauth` in clear text, so only trusted users should be allowed to
run `CONFIG`. See [Security](security.md).

### TLS

TLS applies to the Redis protocol only; memcache is always plain text.

| Option | Type | Default | Runtime | Description |
|---|---|---|---|---|
| `tls-port` | 0–65535 | `0` | no | TLS port next to the plain one. `0` disables TLS. |
| `tls-cert-file` | path | empty | yes | Server certificate chain (PEM). Required with `tls-port`. |
| `tls-key-file` | path | empty | yes | Its private key (PEM). Required with `tls-port`. |
| `tls-ca-cert-file` | path | empty | yes | CAs that sign client certificates. Required unless `tls-auth-clients no`. |
| `tls-auth-clients` | `yes`, `optional`, `no` | `yes` | yes | `yes`: a client must present a certificate signed by those CAs. `optional`: a certificate is verified if the client sends one. `no`: none is asked for. |
| `tls-auth-clients-user` | `off`, `cn` | `off` | yes | `cn`: a verified client certificate logs in as the ACL user named by its common name, without a password. Needs `tls-auth-clients yes` or `optional`. |
| `tls-protocols` | `TLSv1.2`, `TLSv1.3`, or both separated by commas | `TLSv1.2,TLSv1.3` | yes | Allowed protocol versions. |
| `tls-ciphers` | OpenSSL cipher list | empty (OpenSSL default) | yes | TLS 1.2 ciphers. |
| `tls-ciphersuites` | OpenSSL cipher suites | empty (OpenSSL default) | yes | TLS 1.3 cipher suites. |

When a `CONFIG SET` changes any `tls-*` value (except the port), the certificate, key and CA files are read
again into a new TLS context used by new connections; open connections keep the old one. If a file does not
load, the `CONFIG SET` fails and nothing changes. Naming a `tls-*` option is enough, even with the value it
already has, so certificates overwritten in place are rotated with `CONFIG SET tls-cert-file <same path>`.
See [Security](security.md).

### Experimental

| Option | Type | Default | Runtime | Description |
|---|---|---|---|---|
| `experimental-write-path` | `wal`, `wal-buffered`, `no-wal`, `memory` | `wal` | no | For benchmarks of the write path only. `wal` is the only setting that is safe for real data. |

| Value | Effect |
|---|---|
| `wal` | Every write goes to the RocksDB WAL before its reply. |
| `wal-buffered` | The WAL is buffered in memory and written by the maintenance thread once per second; a process crash loses up to about 1 s of writes. |
| `no-wal` | No WAL: only memtable flushes persist data; a crash loses everything still in the memtables. The change feed and replication are unavailable. |
| `memory` | Nothing is written to RocksDB at all; an upper bound for measurements only. |

With anything but `wal`, the server logs `experimental-write-path <value>: writes are NOT durable as
configured` at startup.

## Startup checks

After the file and the command line are applied, these rules are checked; the same checks run on every
`CONFIG SET`:

| Rule | Error |
|---|---|
| `cache-max-object-size` between 64 bytes and 1 MB | `cache-max-object-size must be between 64b and 1mb` |
| `cache-min-memory` not above `cache-max-memory` | `cache-min-memory must not exceed cache-max-memory` |
| `cache-maintenance-interval` at least 10 ms | `cache-maintenance-interval must be at least 10ms` |
| `wal-retention` is 0 or at least 1 s | `wal-retention must be 0 or at least 1s` |
| `fsync always` only with `experimental-write-path wal` | `fsync always requires experimental-write-path wal` |
| Something listens for Redis clients | `port 0 disables the plain port; set tls-port, or nothing listens` |
| `tls-port` has a certificate and key | `tls-port needs tls-cert-file and tls-key-file` |
| Client certificates have CAs | `tls-auth-clients yes/optional needs tls-ca-cert-file (or set tls-auth-clients no)` |
| Login by CN has client certificates | `tls-auth-clients-user cn needs client certificates (tls-auth-clients yes or optional)` |

Errors that only show when the server opens its resources (a port in use, an invalid `bind` address, a
RocksDB directory locked by another process, a certificate that does not load, an invalid `user` rule) are
logged as `fatal: ...` and the process exits with status 1.

## CONFIG GET

```text
CONFIG GET <pattern> [<pattern> ...]
```

Returns the options whose names match any of the glob patterns (case-insensitive), as name/value pairs: a map
in RESP3, a flat array in RESP2. Values use the canonical syntax described under
[Value syntax](#value-syntax).

```text
127.0.0.1:6379> CONFIG GET cache-*
 1) "cache-max-memory"
 2) "512mb"
 3) "cache-min-memory"
 4) "64mb"
...
127.0.0.1:6379> CONFIG GET maxmemory
1) "maxmemory"
2) "536870912"
```

Every option in the tables above is listed except `user` (use `ACL LIST`). For tools written for Redis, four
more names are answered: `databases` (`1`), `save` (empty), `appendonly` (`no`) and `maxmemory`
(`cache-max-memory` in bytes). They cannot be set.

## CONFIG SET

```text
CONFIG SET <option> <value> [<option> <value> ...]
```

- Several pairs can be given at once. They apply atomically: every value is parsed and the resulting
  configuration is validated first, then the changes that can fail (loading TLS files, resizing RocksDB write
  buffers) run before anything else changes. On any error nothing changes.
- Option names are case-insensitive. An option may appear only once per call.
- Each change is logged as `CONFIG SET <option>`.
- Changes are not written to the configuration file; use `CONFIG REWRITE` for that.

The options it accepts are the ones marked "Runtime: yes" above: `fsync`, `write-batching`,
`rocksdb-block-cache`, `rocksdb-write-buffer`, `cache-min-memory`, `cache-max-idle`, `cache-max-age`,
`cache-on-write`, `cache-admission`, `log-level`, `requirepass`, `acllog-max-len`, `masteruser`,
`masterauth`, `backup-dir`, `backup-keep`, `slowlog-log-slower-than`, `slowlog-max-len`, `replica-priority`,
`protected-mode`, and every `tls-*` option except `tls-port`.

Errors:

| Situation | Reply |
|---|---|
| A known option fixed at startup | `ERR CONFIG SET failed (possibly related to argument 'port') - can't set immutable config` |
| An unknown option | `ERR Unknown option or number of arguments for CONFIG SET - 'foo'` |
| The same option twice | `ERR CONFIG SET failed (possibly related to argument 'fsync') - duplicate parameter` |
| A value that does not parse | `ERR CONFIG SET failed (possibly related to argument 'fsync') - invalid value 'sometimes' for 'fsync'` |
| A rule of [Startup checks](#startup-checks) broken | `ERR CONFIG SET failed - cache-min-memory must not exceed cache-max-memory` |
| A TLS file that does not load | `ERR CONFIG SET failed - TLS: cannot load tls-cert-file '...': ...` |
| An odd number of arguments | `ERR wrong number of arguments for 'config\|set' command` |

```text
127.0.0.1:6379> CONFIG SET cache-max-idle 10m fsync always
OK
127.0.0.1:6379> CONFIG SET cache-max-age 1h cache-min-memory 99gb
(error) ERR CONFIG SET failed - cache-min-memory must not exceed cache-max-memory
```

In the second call `cache-max-age` is not changed either.

## CONFIG REWRITE

`CONFIG REWRITE` writes the current settings and ACL users back to the configuration file the server was
started with. Without a file it answers `ERR The server is running without a config file`.

How the file is edited:

1. Comment lines, blank lines and their order stay exactly as they are.
2. A line that sets an option gets the option's current value in place, as `<option> <value>` (lower-case
   name, canonical value). If the option appears on several lines, only the first is kept. If its current
   value is empty (for example `requirepass` cleared with `CONFIG SET`), the line is removed.
3. All `user` lines are replaced, at the position of the first one, by one line per current ACL user, with
   passwords as SHA-256 hashes (`#<hex>`). The `default` user is left out while it has its initial rules
   (`on nopass ~* &* +@all`).
4. Options that were not in the file, are not empty and differ from their defaults are appended at the end,
   after a blank line and the comment `# Generated by CONFIG REWRITE`, followed by the `user` lines if the file
   had none.
5. The new text is written to `<file>.rewrite.tmp` next to the file and renamed over it, so a crash never
   leaves half a file. The server therefore needs write access to the file's directory.

Some consequences:

- The current value includes command-line overrides, so they end up in the file. The Docker image, for
  example, starts with `--dir /var/lib/datadam`, and a rewrite sets the `dir` line to that.
- Rewriting twice in a row produces the same file.
- `REPLICAOF host port` becomes a `replicaof host port` line and `REPLICAOF NO ONE` removes it. A line written
  as `slaveof` is treated the same way, and is rewritten as `replicaof`.

For the Docker image, see [Operations](operations.md#docker-deployment): the server owns `/etc/datadam`, and
the file has to be on a volume to survive re-creating the container.

## CONFIG RESETSTAT

`CONFIG RESETSTAT` clears the latency histograms of `INFO latency` (`storage_get_usec`,
`storage_multiget_usec`, `storage_write_usec`, `key_lock_wait_usec`, `key_read_lock_wait_usec`,
`write_batch_hold_usec` and `read_pool_load_usec`). The counters of `INFO stats`, `INFO cache` and
`INFO persistence` are not reset. See [Operations](operations.md#monitoring).

## Annotated example

A configuration for a server with 8 GB of RAM that holds a data set larger than memory, accepts password-only
clients on the plain port, TLS clients with certificates, and keeps one hour of WAL for a replica and the
change feed. Comments must be on their own lines.

```text
# ---- network ----
# Listen on every interface.
bind 0.0.0.0
port 6379
# memcache is not used by this deployment.
memcache-port 0
# auto (the default): one per CPU up to 8, then half the CPUs.
io-threads auto

# ---- persistence (RocksDB) ----
dir /var/lib/datadam
# Sync the WAL once per second: a power loss loses at most about 1 s.
fsync everysec
# Group commit of pipelined writes.
write-batching yes
# Serves values larger than cache-max-object-size and collection elements.
rocksdb-block-cache 1gb
rocksdb-write-buffer 64mb
rocksdb-background-jobs 4

# ---- change feed / replication source ----
# Keep one hour of WAL, at most 4 GB of it.
wal-retention 1h
wal-retention-size 4gb

# ---- object cache ----
# Hot keys in memory; the rest is read from disk by the read pool.
cache-max-memory 3gb
cache-min-memory 256mb
cache-max-object-size 4kb
cache-max-idle 10m
cache-max-age 0
cache-maintenance-interval 1s
cache-shards 16
cache-on-write yes
cache-admission yes

# ---- read pool ----
read-threads 64
read-batch 1

log-level info

# ---- security ----
requirepass change-me
user app on >app-password ~app:* +@read +@write -@dangerous
user reports on >reports-password %R~* +@read -@dangerous
acllog-max-len 128

# ---- TLS ----
tls-port 6380
tls-cert-file /etc/datadam/server.crt
tls-key-file /etc/datadam/server.key
tls-ca-cert-file /etc/datadam/ca.crt
tls-auth-clients yes
tls-auth-clients-user cn
tls-protocols TLSv1.2,TLSv1.3
```

A replica of this server would add `replicaof <primary-host> 6379` and, since the primary has a password,
`masterauth change-me` (or a `masteruser` that may run `PING`, `REPLCONF` and `REPLSYNC`).
