# memcache protocol

datadam speaks the memcache text protocol on its own port, on the same keyspace as the Redis protocol: a value
stored with memcache `set` is a Redis string, and a Redis string can be read with memcache `get`. This page
lists the commands, how flags, expiration times and CAS values are stored, and the limits.

## Contents

- [Enabling the port](#enabling-the-port)
- [Shared keyspace](#shared-keyspace)
- [Commands](#commands)
- [noreply](#noreply)
- [CAS](#cas)
- [Expiration times](#expiration-times)
- [Limits and error replies](#limits-and-error-replies)
- [Authentication and permissions](#authentication-and-permissions)
- [On a replica](#on-a-replica)
- [stats](#stats)
- [Examples](#examples)
- [Not supported](#not-supported)

## Enabling the port

`memcache-port` (default 11211) sets the port; `memcache-port 0` turns it off. It listens on the same `bind`
address as the Redis port and is served by the same `io-threads`. It is plain TCP: there is no TLS for
memcache. Requests are pipelined as usual, and the writes of the requests read together from one connection
are committed to RocksDB as one batch before their replies are sent, as on the Redis port.

## Shared keyspace

- **Values are Redis strings.** `set`, `add`, `replace` and `cas` store a string record; `GET`, `STRLEN`,
  `APPEND` and the other Redis string commands work on it, and `TYPE` says `string`.
- **Flags** are the 32-bit client flags of the request. They are stored in the record next to the value and
  returned by `get`. Redis commands that replace the value (`SET`, `SETEX`, `GETSET`, `MSET`...) store flags
  0; commands that change it in place (`APPEND`, `SETRANGE`, `INCR`...) keep them. Redis clients never see
  the flags.
- **Expiration times** become the key's TTL, in milliseconds: `TTL` and `PTTL` show it, and `EXPIRE` or
  `PERSIST` from Redis change it for memcache too.
- **Keys of other types.** A key holding a hash, set, sorted set, list or JSON document is invisible to
  `get`, `gets`, `gat` and `gats` (it is reported as a miss). `replace`, `append` and `prepend` reply
  `NOT_STORED`, `cas` replies `NOT_FOUND`, and `incr`/`decr` reply `NOT_FOUND`. `set` replaces such a key with
  a string, as Redis `SET` does, and so does `add`: it treats a key of another type as missing and
  overwrites it. `delete` and `touch` work on keys of any type.
- **Indexes.** A memcache write over a JSON document or a hash keeps [secondary indexes](secondary-indexes.md)
  and [search indexes](search.md) up to date, like any other write.

## Commands

Keys are separated from the other tokens by spaces or tabs. Lines end with `\r\n` (a bare `\n` is also
accepted).

### Storage commands

```text
set <key> <flags> <exptime> <bytes> [noreply]\r\n<data>\r\n
add <key> <flags> <exptime> <bytes> [noreply]\r\n<data>\r\n
replace <key> <flags> <exptime> <bytes> [noreply]\r\n<data>\r\n
append <key> <flags> <exptime> <bytes> [noreply]\r\n<data>\r\n
prepend <key> <flags> <exptime> <bytes> [noreply]\r\n<data>\r\n
cas <key> <flags> <exptime> <bytes> <cas unique> [noreply]\r\n<data>\r\n
```

| Command | Stores when | Reply |
|---|---|---|
| `set` | always | `STORED` |
| `add` | the key does not hold a string | `STORED`, or `NOT_STORED` |
| `replace` | the key holds a string | `STORED`, or `NOT_STORED` |
| `append`, `prepend` | the key holds a string; the data is added after / before the value | `STORED`, or `NOT_STORED` |
| `cas` | the key holds a string whose CAS value equals `<cas unique>` | `STORED`; `EXISTS` if the CAS value differs; `NOT_FOUND` if the key does not hold a string |

`set`, `add`, `replace` and `cas` store the given flags and expiration time. `append` and `prepend` ignore
them and keep the value's current flags and TTL.

### Retrieval commands

```text
get <key> [<key> ...]\r\n
gets <key> [<key> ...]\r\n
gat <exptime> <key> [<key> ...]\r\n
gats <exptime> <key> [<key> ...]\r\n
```

Each hit is returned as `VALUE <key> <flags> <bytes>\r\n<data>\r\n` (`gets` and `gats` add the CAS value:
`VALUE <key> <flags> <bytes> <cas unique>`), followed by `END\r\n`. Misses, expired keys and keys of other
types are left out. `gat` and `gats` ("get and touch") also set the expiration time of every string they
find, without changing its CAS value; with an `exptime` in the past the key is deleted and not returned.
`get` with no key replies `ERROR`.

### Other commands

| Command | Syntax | Reply |
|---|---|---|
| `delete` | `delete <key> [0] [noreply]` | `DELETED` or `NOT_FOUND`. The legacy time argument is accepted only as `0`. Works on any key type. |
| `incr`, `decr` | `incr <key> <delta> [noreply]` | The new value. `delta` and the stored value are unsigned 64-bit decimals. `incr` wraps around at 2^64; `decr` stops at 0. `NOT_FOUND` if the key does not hold a string. Flags and TTL are kept; the CAS value changes. |
| `touch` | `touch <key> <exptime> [noreply]` | `TOUCHED` or `NOT_FOUND`. Sets the expiration time of a key of any type, without changing its CAS value. |
| `flush_all` | `flush_all [delay] [noreply]` | `OK`. Deletes every key at once, like Redis `FLUSHALL`. `delay` must be an integer and is ignored. |
| `version` | `version` | `VERSION <datadam version>`, e.g. `VERSION 0.1.0`. |
| `verbosity` | `verbosity [...] [noreply]` | `OK`. Has no effect. |
| `stats` | `stats [...]` | The general statistics (see [stats](#stats)), whatever the arguments. |
| `auth` | `auth [user] <password>` | `OK`. See [Authentication and permissions](#authentication-and-permissions). |
| `quit` | `quit` | Closes the connection without a reply. |

An unknown command or an empty line replies `ERROR`.

## noreply

A trailing `noreply` suppresses the reply of `set`, `add`, `replace`, `append`, `prepend`, `cas`, `delete`,
`incr`, `decr`, `touch`, `flush_all` and `verbosity`. Errors about the request itself (`ERROR`,
`CLIENT_ERROR bad command line format`, `CLIENT_ERROR invalid numeric delta argument`...) and permission errors
are still sent; `CLIENT_ERROR cannot increment or decrement non-numeric value` is suppressed like a normal
reply. `get`, `gets`, `gat`, `gats`, `stats` and `version` always reply.

## CAS

Every record carries a 64-bit CAS value that changes whenever its value changes, through either protocol:
memcache `set`/`add`/`replace`/`append`/`prepend`/`cas`/`incr`/`decr`, and Redis commands such as `SET`,
`APPEND`, `INCR` or `RENAME`. Changes to the expiration time only (`touch`, `gat`, `gats`, Redis `EXPIRE`,
`PERSIST`, `GETEX`) keep it. So a `cas` fails with `EXISTS` when anything, Redis client or memcache client,
changed the value since the `gets`.

CAS values come from one counter for the whole server, started from the current time (in milliseconds,
times 1,000) when the server starts, so they keep increasing across restarts. They are stored with the
record, so a restart does not change the CAS value of existing keys.

## Expiration times

`exptime` follows memcache:

| `exptime` | Meaning |
|---|---|
| `0` | Never expires. |
| 1 to 2,592,000 (30 days) | Seconds from now. |
| more than 2,592,000 | An absolute Unix time in seconds. A time in the past expires the key at once. |
| negative | Expires the key at once. |

An expired key is invisible immediately and removed later, as on the Redis side.

## Limits and error replies

| Limit | Value | When exceeded |
|---|---|---|
| Key length | 1 to 250 bytes, no control characters, spaces or DEL (byte 127) | `CLIENT_ERROR bad command line format` (storage commands) |
| Value size | 512 MB | `SERVER_ERROR object too large for cache`, and the connection is closed |
| Command line | 64 KB without a newline | `CLIENT_ERROR line too long`, and the connection is closed |
| Flags | 32-bit unsigned | `CLIENT_ERROR bad command line format` |

Other errors:

- A storage command whose header is malformed but whose `<bytes>` can be read consumes the data block and
  replies `CLIENT_ERROR bad command line format`. If `<bytes>` cannot be read, only the line is consumed.
- A data block not followed by `\r\n` replies `CLIENT_ERROR bad data chunk` and closes the connection.
- `incr`/`decr`: `CLIENT_ERROR invalid numeric delta argument`, or
  `CLIENT_ERROR cannot increment or decrement non-numeric value` when the stored value is not an unsigned
  integer. A wrong number of arguments replies `ERROR`.
- `delete` with bad arguments: `CLIENT_ERROR bad command line format.  Usage: delete <key> [noreply]`.
- `gat`, `gats`, `touch` and `flush_all` with bad arguments: `CLIENT_ERROR bad command line format`.
- A storage failure replies `SERVER_ERROR <message>`.

## Authentication and permissions

The memcache text protocol has no authentication of its own, so datadam adds an `auth` command. When the
Redis `default` user needs no password, memcache connections start logged in as `default`. When it has one
(`requirepass`) or is `off`, a connection must first send:

```text
auth <password>            logs in as the default user
auth <user> <password>     logs in as an ACL user
```

`auth` replies `OK` or `CLIENT_ERROR authentication failed` (counted in `auth_failures` and recorded in
`ACL LOG`); a wrong number of arguments replies `CLIENT_ERROR usage: auth [user] <password>`. `auth` can
also switch users on a connection that is already logged in. Before logging in, only `version` and `quit`
are accepted: anything else replies `CLIENT_ERROR authentication required (send: auth <password>)` and closes
the connection, because the data block of a storage command would otherwise be read as commands.

The password is sent in clear text; keep the memcache port on a trusted network, or turn it off.

A logged-in connection has the permissions of its ACL user, checked like the equivalent Redis command, keys
included:

| memcache command | Checked as | Key access |
|---|---|---|
| `get`, `gets` | `MGET` | read |
| `gat`, `gats` | `GETEX` | write |
| `set`, `replace`, `cas` | `SET` | write |
| `add` | `SETNX` | write |
| `append`, `prepend` | `APPEND` | write |
| `delete` | `DEL` | write |
| `incr`, `decr` | `INCRBY` | write |
| `touch` | `EXPIRE` | write |
| `flush_all` | `FLUSHALL` | none |
| `stats` | `INFO` | none |
| `version`, `verbosity`, `quit` | not checked | none |

A refusal replies `CLIENT_ERROR NOPERM user <name> may not run '<command>'` or
`CLIENT_ERROR NOPERM no permissions to access a key`, counts in `acl_access_denied` and is recorded in
`ACL LOG` with `client-info` `protocol=memcache`. If the user changes, the connection uses the new
permissions from its next command; if the user is deleted, the next command replies
`SERVER_ERROR the user of this connection was deleted` and the connection is closed. See
[Security](security.md).

## On a replica

A [replica](replication.md) serves `get`, `gets` and `stats`. Commands that write (every storage command,
`gat`, `gats`, `delete`, `incr`, `decr`, `touch` and `flush_all`) reply `SERVER_ERROR read only replica`;
a storage command's data block is consumed first.

## stats

`stats` replies with these lines, then `END`:

| Field | Value |
|---|---|
| `pid` | Process id. |
| `uptime` | Seconds since the server started. |
| `time` | Current Unix time. |
| `version` | datadam version. |
| `threads` | `io-threads`. |
| `curr_connections` | Open connections, Redis and memcache together. |
| `total_connections` | Connections accepted since start, both protocols. |
| `curr_items` | Estimated number of keys (the same estimate as `DBSIZE`). |
| `limit_maxbytes` | `cache-max-memory`. |
| `bytes` | Bytes of keys and values in the object cache. |
| `cache_items` | Items in the object cache. |
| `get_hits`, `get_misses` | Object cache hits and misses, for all reads from both protocols (a miss is a read that went to RocksDB, not necessarily a missing key). |
| `evictions` | Items evicted from the object cache (LRU, idle and age). |

`INFO` and `CACHE STATS` on the Redis port give more detail; see [Operations](operations.md).

## Examples

Each example sends requests with `printf` and ends with `quit`, so that `nc` exits when the server closes the
connection.

```bash
$ printf 'set greeting 42 0 5\r\nhello\r\nget greeting\r\nquit\r\n' | nc localhost 11211
STORED
VALUE greeting 42 5
hello
END
```

The same key from Redis, and a counter shared by both protocols:

```bash
$ redis-cli GET greeting
"hello"
$ redis-cli SET visits 10
OK
$ printf 'incr visits 5\r\nquit\r\n' | nc localhost 11211
15
$ redis-cli INCR visits
(integer) 16
```

Check-and-set: read the CAS value with `gets`, then write only if nothing changed it.

```bash
$ printf 'gets greeting\r\nquit\r\n' | nc localhost 11211
VALUE greeting 42 5 1761210000123001
hello
END
$ printf 'cas greeting 42 0 3 1761210000123001\r\nbye\r\nquit\r\n' | nc localhost 11211
STORED
$ printf 'cas greeting 42 0 3 1761210000123001\r\nbye\r\nquit\r\n' | nc localhost 11211
EXISTS
```

Expiration times and `noreply`:

```bash
$ printf 'set session 0 60 2 noreply\r\nok\r\ntouch session 3600\r\nquit\r\n' | nc localhost 11211
TOUCHED
$ redis-cli TTL session
(integer) 3600
```

With a password:

```bash
$ printf 'get greeting\r\n' | nc localhost 11211
CLIENT_ERROR authentication required (send: auth <password>)
$ printf 'auth s3cret\r\nget greeting\r\nquit\r\n' | nc localhost 11211
OK
VALUE greeting 42 3
bye
END
```

## Not supported

- The binary protocol and the meta commands (`mg`, `ms`, `md`, `ma`, `mn`, `me`).
- `stats` subcommands (`stats items`, `stats slabs`, `stats settings`...): `stats` with arguments returns the
  general statistics.
- `shutdown`, `cache_memlimit`, `slabs`, `lru_crawler`, `watch` and the other administration commands: they
  reply `ERROR`.
- A delayed `flush_all`: the delay is ignored and the flush is immediate.
- SASL authentication, TLS and UDP.
