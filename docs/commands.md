# Command reference

This page lists every Redis-protocol command datadam implements, with the syntax its parser accepts and the
places where it behaves differently from Redis. Commands for JSON, indexes, search, the change feed,
replication and ACL get one line each here and are covered in depth on their own pages.

## Contents

- [Conventions](#conventions)
- [Protocol and limits](#protocol-and-limits)
- [Connection and server](#connection-and-server)
- [Strings](#strings)
- [Keys and expiration](#keys-and-expiration)
- [SCAN family](#scan-family)
- [Hashes](#hashes)
- [Sets](#sets)
- [Sorted sets](#sorted-sets)
- [Lists](#lists)
- [Blocking commands](#blocking-commands)
- [Transactions](#transactions)
- [Pub/Sub](#pubsub)
- [Scripting](#scripting)
- [JSON](#json)
- [Secondary indexes](#secondary-indexes)
- [Search](#search)
- [Change feed](#change-feed)
- [Replication](#replication)
- [ACL](#acl)
- [Differences from Redis at a glance](#differences-from-redis-at-a-glance)
- [Unsupported commands](#unsupported-commands)

## Conventions

- Syntax uses the Redis notation: `UPPERCASE` words are keywords, `[x]` is optional, `a|b` is a choice and
  `...` repeats the previous item. Command names and keywords are case-insensitive; keys and values are not.
- "Collection" means a hash, set, sorted set or list. Every collection command checks the type of its key
  and replies `WRONGTYPE Operation against a key holding the wrong kind of value` when it holds another type.
  A missing key behaves like an empty collection, and a collection whose last element is removed is deleted.
- Every command runs atomically on its keys. Multi-key commands (`MSET`, `SMOVE`, `SINTERSTORE`, `LMOVE`,
  `RENAME`...) lock all their keys and commit their writes as one RocksDB batch.
- The ACL categories of a command (`@read`, `@write`, `@dangerous`...) are listed by `ACL CAT <category>`; see
  [Security](security.md#categories).

Errors that any command can return:

| Error | When |
|---|---|
| `ERR unknown command 'X', with args beginning with: ...` | The command does not exist (see [Unsupported commands](#unsupported-commands)). |
| `ERR wrong number of arguments for 'x' command` | Too few or too many arguments. |
| `NOAUTH Authentication required.` | The connection has not logged in yet ([Security](security.md)). |
| `NOPERM ...` | The ACL user may not run the command or touch one of its keys. |
| `READONLY You can't write against a read only replica.` | A write on a [replica](replication.md). |
| `LOADING datadam is loading the dataset from its primary` | A data command on a replica during a full sync. |
| `ERR storage error: ...` | RocksDB failed the operation. |

## Protocol and limits

- **RESP2 and RESP3.** A connection starts in RESP2. `HELLO 3` switches it to RESP3, where replies use the
  RESP3 types: maps (`HGETALL`, `CONFIG GET`, `CACHE STATS`, `HELLO`, `ACL GETUSER`, `ACL LOG`, `IDX.INFO`,
  `FT.INFO`, `CDC.INFO`), sets (`SMEMBERS`, `SINTER`, `SUNION`, `SDIFF`, `SPOP` with a count), doubles (sorted
  set scores) and the null `_`. In RESP3, replies with scores or values (`ZRANGE ... WITHSCORES`, `ZPOPMIN`
  with a count, `HRANDFIELD ... WITHVALUES`) are arrays of `[member, score]` pairs instead of a flat array.
  Pub/Sub messages and confirmations are RESP3 pushes (`>`). `HELLO 2` switches back.
- **Inline commands.** A line that does not start with `*` is split on spaces, as in Redis, so
  `printf 'PING\r\n' | nc localhost 6379` works. Inline lines are limited to 64 KB.
- **Sizes.** A bulk argument may be up to 512 MB and a request may have up to 1,048,576 arguments. Strings are
  limited to 512 MB (`APPEND` and `SETRANGE` reply
  `ERR string exceeds maximum allowed size (proto-max-bulk-len)`).
- **One database.** Only database 0 exists.
- **Pipelining.** The writes of all requests read together from one connection are committed to RocksDB as
  one batch before any of their replies is sent (`write-batching yes`); see
  [Operations](operations.md).
- **Before login.** When a password is required, only `AUTH`, `HELLO` and `QUIT` are accepted, and a request
  larger than 16 KB closes the connection ([Security](security.md#passwords-requirepass-and-auth)).

## Connection and server

| Command | Syntax | Notes |
|---|---|---|
| PING | `PING [message]` | `PONG`, or the message as a bulk string. |
| ECHO | `ECHO message` | |
| QUIT | `QUIT` | Replies `OK` and closes the connection. |
| SELECT | `SELECT index` | Only `0` is accepted; any other index replies `ERR DB index is out of range`. |
| AUTH | `AUTH [username] password` | Logs in as `username` (default: `default`). See [Security](security.md). |
| HELLO | `HELLO [protover [AUTH username password] [SETNAME clientname]]` | `protover` is 2 or 3 (`NOPROTO unsupported protocol version` otherwise). Replies with a map: `server` `datadam`, `version` `7.2.0`, `proto`, `id`, `mode` `standalone`, `role` `master` (also on a replica), `modules` (empty). |
| CLIENT | `CLIENT SETNAME name` | Names may not contain spaces or newlines. |
| | `CLIENT GETNAME` | Nil when no name was set. |
| | `CLIENT ID` | |
| | `CLIENT SETINFO LIB-NAME\|LIB-VER value` | |
| | `CLIENT INFO` | The calling connection: `id addr name db resp user tls lib-name lib-ver`. |
| | `CLIENT LIST [TYPE normal\|pubsub\|replica\|master] [ID id ...]` | Every Redis connection, one line each: `id addr name age idle flags db sub psub ssub multi cmd user resp lib-name lib-ver tls`. |
| | `CLIENT KILL ip:port` | Closes that connection; `OK` or `ERR No such client`. |
| | `CLIENT KILL [ID id] [ADDR ip:port] [USER name] [TYPE type] [MAXAGE seconds] [SKIPME yes\|no]` | Closes every connection that matches all the filters (not this one, unless `SKIPME no`); replies how many. |
| | `CLIENT NO-EVICT\|NO-TOUCH\|REPLY arg` | Accepted for client-library compatibility; reply `OK` and have no effect (`CLIENT REPLY OFF` does not silence replies). |
| COMMAND | `COMMAND [COUNT \| LIST \| DOCS \| INFO name ...]` | `COUNT` and `LIST` are exact. `COMMAND` alone returns an empty array, `DOCS` an empty map and `INFO` a nil per name: datadam has no command documentation table. |
| CONFIG | `CONFIG GET pattern [pattern ...]` | Glob patterns, case-insensitive. Besides datadam's own options it answers `databases` (1), `save` (empty), `appendonly` (`no`) and `maxmemory` (the value of `cache-max-memory`). |
| | `CONFIG SET parameter value [parameter value ...]` | All pairs are validated first; either all apply or none. Only some options can change at run time; see [Configuration](configuration.md). Others reply `can't set immutable config`. |
| | `CONFIG REWRITE` | Writes the current settings and ACL users into the config file the server started with. |
| | `CONFIG RESETSTAT` | Clears the `INFO latency` histograms. Counters in `INFO stats` are not reset. |
| INFO | `INFO [section]` | Sections: `server`, `clients`, `stats`, `cache` (alias `memory`), `persistence`, `latency`, `replication`, `keyspace`; none, `all`, `everything` or `default` gives all. Only the first argument is used. See [Operations](operations.md). |
| TIME | `TIME` | Unix seconds and microseconds. |
| DBSIZE | `DBSIZE` | **An estimate** (RocksDB's `rocksdb.estimate-num-keys`); it can count expired and deleted keys that compaction has not removed yet. `INFO keyspace` shows the same number, with `expires=0,avg_ttl=0`. |
| FLUSHDB | `FLUSHDB [ASYNC\|SYNC]` | Same as `FLUSHALL`. |
| FLUSHALL | `FLUSHALL [ASYNC\|SYNC]` | Deletes every key and every index entry, always synchronously. Index definitions remain and keep being maintained. |
| SAVE | `SAVE` | Commits pending writes and flushes the RocksDB memtables to SST files. With the default write path every write is already in the WAL; this only shortens recovery. |
| BGSAVE | `BGSAVE [SCHEDULE]` | With `backup-dir` set, writes a backup (a RocksDB checkpoint) there in the background, keeping the newest `backup-keep`. Without it, the same as `SAVE`. Replies `Background saving started`. See [Operations](operations.md#backups). |
| BACKUP | `BACKUP path` | datadam's own: a consistent copy of the database at `path` (which must not exist), made while the server runs; replies when it is complete. |
| LASTSAVE | `LASTSAVE` | Unix time of the last successful `SAVE`, `BGSAVE` or `BACKUP`. |
| SHUTDOWN | `SHUTDOWN [NOSAVE\|SAVE] [NOW] [FORCE] [ABORT]` | Stops the server cleanly; no reply. `SAVE` flushes first. `ABORT` replies `ERR No shutdown in progress.`. |
| SLOWLOG | `SLOWLOG GET [count] \| LEN \| RESET \| HELP` | Commands slower than `slowlog-log-slower-than`; see [Operations](operations.md#other-sources). |
| MONITOR | `MONITOR` | Streams every command other connections run, with passwords hidden. |
| CACHE | `CACHE STATS` | datadam's own command: a map with the object cache settings and counters (`max_memory`, `min_memory`, `max_object_size`, `max_idle_ms`, `max_age_ms`, `page_size`, `pages_in_use`, `pages_committed`, `used_memory`, `chunk_bytes`, `payload_bytes`, `items`, `hits`, `misses`, `inserts`, `rejected_too_large`, `rejected_admission`, `alloc_failures`, `evicted_lru`, `evicted_idle`, `evicted_age`, `evicted_ttl`, `page_steals`). |
| | `CACHE CLASSES` | One map per slab size class: `chunk_size`, `pages`, `items`. |
| | `CACHE CLEAR` | Empties the object cache. No data is lost: later reads load from RocksDB. |

`HELP` subcommands (`CLIENT HELP`, `CONFIG HELP`, `ACL HELP`...) are not implemented, although some error
messages suggest them.

## Strings

| Command | Syntax | Notes |
|---|---|---|
| GET | `GET key` | |
| SET | `SET key value [NX\|XX] [GET] [EX seconds\|PX milliseconds\|EXAT unix-seconds\|PXAT unix-milliseconds\|KEEPTTL]` | Options in any order. Expiration values must be positive (`ERR invalid expire time in 'set' command`). `NX` with `XX`, or two expiration options, is a syntax error. Without `NX`/`XX`/`GET`, `SET` overwrites a key of any type. With `GET` on a non-string key it replies `WRONGTYPE` and writes nothing. |
| SETNX | `SETNX key value` | 1 if written, 0 if the key exists. |
| SETEX | `SETEX key seconds value` | Seconds must be positive. |
| PSETEX | `PSETEX key milliseconds value` | |
| GETSET | `GETSET key value` | |
| GETDEL | `GETDEL key` | |
| GETEX | `GETEX key [EX seconds\|PX milliseconds\|EXAT unix-seconds\|PXAT unix-milliseconds\|PERSIST]` | At most one option. |
| MGET | `MGET key [key ...]` | Nil for missing keys and for keys of other types. |
| MSET | `MSET key value [key value ...]` | One atomic batch. |
| MSETNX | `MSETNX key value [key value ...]` | Writes nothing (and replies 0) if any key exists. |
| APPEND | `APPEND key value` | Creates the key if missing. |
| STRLEN | `STRLEN key` | |
| GETRANGE | `GETRANGE key start end` | Negative offsets count from the end. |
| SUBSTR | `SUBSTR key start end` | Alias of `GETRANGE`. |
| SETRANGE | `SETRANGE key offset value` | Pads with zero bytes. An empty value does not create the key. |
| INCR, DECR | `INCR key`, `DECR key` | 64-bit signed. `ERR increment or decrement would overflow` on overflow. |
| INCRBY, DECRBY | `INCRBY key increment`, `DECRBY key decrement` | |
| INCRBYFLOAT | `INCRBYFLOAT key increment` | Computed in `long double`; NaN or infinite results are refused. |

Counter and `APPEND`/`SETRANGE` writes keep the key's TTL; `SET`, `SETEX` and `GETSET` replace it. Strings
written over the memcache port are ordinary strings here; see [memcache](memcache.md).

## Keys and expiration

| Command | Syntax | Notes |
|---|---|---|
| DEL | `DEL key [key ...]` | O(1) per key, also for collections: their elements become invisible at once and are reclaimed later. |
| UNLINK | `UNLINK key [key ...]` | Same as `DEL`. |
| EXISTS | `EXISTS key [key ...]` | Counts a key once per time it is named. |
| TYPE | `TYPE key` | `string`, `hash`, `set`, `zset`, `list`, `ReJSON-RL` (a JSON document) or `none`. |
| EXPIRE | `EXPIRE key seconds [NX\|XX\|GT\|LT]` | Options can be combined (`XX GT`), except `NX` with any other and `GT` with `LT`. Other words reply `ERR Unsupported option <word>`. A time in the past deletes the key and replies 1. For `GT`, a key without a TTL counts as infinite. |
| PEXPIRE | `PEXPIRE key milliseconds [NX\|XX\|GT\|LT]` | |
| EXPIREAT | `EXPIREAT key unix-seconds [NX\|XX\|GT\|LT]` | |
| PEXPIREAT | `PEXPIREAT key unix-milliseconds [NX\|XX\|GT\|LT]` | |
| TTL, PTTL | `TTL key`, `PTTL key` | -2 when the key does not exist, -1 when it has no TTL. `TTL` rounds to the nearest second. |
| EXPIRETIME, PEXPIRETIME | `EXPIRETIME key`, `PEXPIRETIME key` | Absolute expiration time; -2 / -1 as above. |
| PERSIST | `PERSIST key` | |
| RENAME | `RENAME key newkey` | Keeps the TTL. Overwrites `newkey` whatever its type. `ERR no such key` when `key` is missing. |
| RENAMENX | `RENAMENX key newkey` | 0 if `newkey` exists. |
| TOUCH | `TOUCH key [key ...]` | How many of the keys exist; marks them as just used in the object cache. |
| OBJECT | `OBJECT ENCODING key` | The name Redis would use for such a value: `int`, `embstr` or `raw` for strings (by content and size), `listpack` for collections of up to 128 elements, else `hashtable` (hash, set), `skiplist` (zset) or `quicklist` (list); `raw` for JSON. Nil for a missing key. datadam's own storage is the same for all of them (see [Storage format](dev/storage-format.md)). |
| | `OBJECT IDLETIME key` | Seconds since the cached copy was last read; 0 when the key is not in the object cache. |
| | `OBJECT REFCOUNT key` | Always 1. `OBJECT FREQ` replies the Redis error for a non-LFU policy; `OBJECT HELP`. |
| MEMORY | `MEMORY USAGE key [SAMPLES count]` | An estimate in bytes: the record, plus for a collection the average size of `count` sampled elements (default 5; 0 reads them all) times the number of elements. Nil for a missing key. Only `USAGE` and `HELP`. |
| KEYS | `KEYS pattern` | Reads the whole keyspace from RocksDB; prefer `SCAN`. For an ACL user with key patterns, only the keys it may read are returned. |

Patterns (in `KEYS`, `SCAN ... MATCH`, ACL key rules and `CDC.READ MATCH`) are Redis globs: `*`, `?`,
`[abc]`, `[^abc]`, `[a-z]` and `\` to escape.

An expired key is invisible from the moment it expires. It is removed when it is next read (on a primary) or
by the RocksDB compaction filter, whichever comes first. Every key type, collections and JSON documents
included, can have a TTL.

## SCAN family

| Command | Syntax | Notes |
|---|---|---|
| SCAN | `SCAN cursor [MATCH pattern] [COUNT count] [TYPE type]` | Keys come in byte order. |
| HSCAN | `HSCAN key cursor [MATCH pattern] [COUNT count] [NOVALUES]` | Field and value pairs, in field order. |
| SSCAN | `SSCAN key cursor [MATCH pattern] [COUNT count]` | Members in byte order. |
| ZSCAN | `ZSCAN key cursor [MATCH pattern] [COUNT count]` | Member and score pairs, in member byte order (not score order). |

The family differs from Redis in several ways:

- **Exactly once.** Keys and elements are stored in order, so each call resumes right after the last item it
  looked at. Every item present for the whole iteration is returned exactly once, never twice. (Redis only
  promises at least once.) Items added or removed during the iteration may or may not appear.
- **COUNT** is the number of keys or elements looked at per call (default 10, capped at 1,000,000). With
  `MATCH`, `TYPE` or ACL filtering, a page can be empty while the cursor is not 0; keep calling until the
  cursor is 0. `COUNT 0` or a negative count is a syntax error.
- **MATCH with a literal prefix** (`user:*`) only walks the keys with that prefix, so it costs the same as a
  scan of those keys alone.
- **TYPE** takes `string`, `hash`, `set`, `zset` or `list`, case-insensitively. JSON documents cannot be
  selected with `TYPE`: the argument is lower-cased before it is compared with `ReJSON-RL`, so it never
  matches.
- **Cursors** are random numbers that stand for a position kept in server memory. Any connection can continue
  any cursor, and using a cursor twice continues from the same place twice. The 100,000 most recently used
  cursors are kept; a forgotten one, one from before a restart, or one used with another key or command
  replies `ERR invalid cursor (unknown or expired; start again from 0)`. A cursor that is not a number replies
  `ERR invalid cursor`.
- **ACL.** `SCAN` returns only the keys the logged-in user may read.
- `HSCAN`/`SSCAN`/`ZSCAN` on a missing key return cursor `0` and no elements.

## Hashes

| Command | Syntax | Notes |
|---|---|---|
| HSET | `HSET key field value [field value ...]` | Number of new fields. |
| HMSET | `HMSET key field value [field value ...]` | Replies `OK`. |
| HSETNX | `HSETNX key field value` | |
| HGET | `HGET key field` | |
| HMGET | `HMGET key field [field ...]` | |
| HDEL | `HDEL key field [field ...]` | |
| HEXISTS | `HEXISTS key field` | |
| HLEN | `HLEN key` | O(1). |
| HSTRLEN | `HSTRLEN key field` | |
| HGETALL | `HGETALL key` | Fields in byte order (not insertion order). A map in RESP3. |
| HKEYS, HVALS | `HKEYS key`, `HVALS key` | Field byte order. |
| HINCRBY | `HINCRBY key field increment` | `ERR hash value is not an integer` if the field is not an integer. |
| HINCRBYFLOAT | `HINCRBYFLOAT key field increment` | `ERR hash value is not a float` if the field is not a number. |
| HRANDFIELD | `HRANDFIELD key [count [WITHVALUES]]` | Reads the whole hash (O(N)). A positive count gives distinct fields, a negative one may repeat them (at most 1,000,000,000). |
| HSCAN | see [SCAN family](#scan-family) | |

Writes to a hash keep its TTL. Hashes can be indexed by [secondary indexes](secondary-indexes.md) and
[search indexes](search.md); every hash write maintains them in the same batch, and a write that would break a
`UNIQUE` index fails as a whole with `ERR UNIQUE index '<name>' already has this value, for key '<key>'`.

## Sets

| Command | Syntax | Notes |
|---|---|---|
| SADD | `SADD key member [member ...]` | |
| SREM | `SREM key member [member ...]` | |
| SISMEMBER | `SISMEMBER key member` | |
| SMISMEMBER | `SMISMEMBER key member [member ...]` | |
| SCARD | `SCARD key` | O(1). |
| SMEMBERS | `SMEMBERS key` | Members in byte order. A set in RESP3. |
| SPOP | `SPOP key [count]` | Reads the whole set (O(N)). A negative count replies `ERR value is out of range, must be positive`. |
| SRANDMEMBER | `SRANDMEMBER key [count]` | Reads the whole set (O(N)). A negative count may repeat members (at most 1,000,000,000). |
| SMOVE | `SMOVE source destination member` | Both keys must be sets (or missing). |
| SINTER, SUNION, SDIFF | `SINTER key [key ...]` (same for the others) | |
| SINTERSTORE, SUNIONSTORE, SDIFFSTORE | `SINTERSTORE destination key [key ...]` (same for the others) | The destination is overwritten whatever its type; an empty result deletes it. The source keys must be sets. |
| SINTERCARD | `SINTERCARD numkeys key [key ...] [LIMIT limit]` | Computes the full intersection, then applies `LIMIT`. Errors: `ERR numkeys should be greater than 0`, `ERR Number of keys can't be greater than number of args`, `ERR LIMIT can't be negative`. |
| SSCAN | see [SCAN family](#scan-family) | |

## Sorted sets

| Command | Syntax | Notes |
|---|---|---|
| ZADD | `ZADD key [NX\|XX] [GT\|LT] [CH] [INCR] score member [score member ...]` | Options in any order, before the pairs. `INCR` takes one pair and replies the new score (nil if `NX`/`XX`/`GT`/`LT` prevented it). |
| ZINCRBY | `ZINCRBY key increment member` | |
| ZREM | `ZREM key member [member ...]` | |
| ZSCORE | `ZSCORE key member` | |
| ZMSCORE | `ZMSCORE key member [member ...]` | |
| ZCARD | `ZCARD key` | O(1). |
| ZCOUNT | `ZCOUNT key min max` | Walks the range: O(M) for M matches, where Redis is O(log N). |
| ZLEXCOUNT | `ZLEXCOUNT key min max` | O(M), as `ZCOUNT`. |
| ZRANGE | `ZRANGE key start stop [BYSCORE\|BYLEX] [REV] [LIMIT offset count] [WITHSCORES]` | With `REV`, give the larger bound first. `LIMIT` needs `BYSCORE` or `BYLEX`; `WITHSCORES` cannot be used with `BYLEX`. A negative `LIMIT` count means all. |
| ZRANGEBYSCORE | `ZRANGEBYSCORE key min max [WITHSCORES] [LIMIT offset count]` | |
| ZREVRANGEBYSCORE | `ZREVRANGEBYSCORE key max min [WITHSCORES] [LIMIT offset count]` | |
| ZREVRANGE | `ZREVRANGE key start stop [WITHSCORES]` | |
| ZRANGEBYLEX | `ZRANGEBYLEX key min max [LIMIT offset count]` | |
| ZREVRANGEBYLEX | `ZREVRANGEBYLEX key max min [LIMIT offset count]` | |
| ZRANK, ZREVRANK | `ZRANK key member [WITHSCORE]` (same for `ZREVRANK`) | O(rank): walks the members before it. |
| ZREMRANGEBYSCORE | `ZREMRANGEBYSCORE key min max` | |
| ZREMRANGEBYRANK | `ZREMRANGEBYRANK key start stop` | |
| ZREMRANGEBYLEX | `ZREMRANGEBYLEX key min max` | |
| ZPOPMIN, ZPOPMAX | `ZPOPMIN key [count]` (same for `ZPOPMAX`) | |
| ZMPOP | `ZMPOP numkeys key [key ...] MIN\|MAX [COUNT count]` | From the first sorted set that has members: `[key, [[member, score] ...]]`; nil if none has. |
| BZPOPMIN, BZPOPMAX | `BZPOPMIN key [key ...] timeout` (same for `BZPOPMAX`) | `[key, member, score]`; waits if every set is empty. See [Blocking commands](#blocking-commands). |
| BZMPOP | `BZMPOP timeout numkeys key [key ...] MIN\|MAX [COUNT count]` | `ZMPOP` that waits. |
| ZSCAN | see [SCAN family](#scan-family) | |

Notes:

- **Scores** are doubles. `inf`, `+inf` and `-inf` are accepted; a result that is NaN replies
  `ERR resulting score is not a number (NaN)`. Score bounds take `(` for an exclusive bound
  (`ERR min or max is not a float` otherwise).
- **Lex ranges** take `[` (inclusive), `(` (exclusive), `-` and `+`
  (`ERR min or max not valid string range item` otherwise). They always use the byte order of the members,
  whatever their scores. In Redis the result is only defined when all members have the same score.
- **Costs.** Rank-based operations (`ZRANK`, `ZRANGE` by index, `ZREMRANGEBYRANK`, `ZPOPMIN`/`ZPOPMAX`) walk
  the members in score order up to the position asked for, so they cost O(rank), not O(log N). Score and lex
  ranges seek straight to their start.
- `ZADD` option errors: `ERR XX and NX options at the same time are not compatible`,
  `ERR GT, LT, and/or NX options at the same time are not compatible`,
  `ERR INCR option supports a single increment-element pair`.

## Lists

| Command | Syntax | Notes |
|---|---|---|
| LPUSH, RPUSH | `LPUSH key element [element ...]` (same for `RPUSH`) | O(1) per element at either end. |
| LPUSHX, RPUSHX | `LPUSHX key element [element ...]` (same for `RPUSHX`) | Only if the list exists. |
| LPOP, RPOP | `LPOP key [count]` (same for `RPOP`) | With a count on a missing key: nil array. |
| LLEN | `LLEN key` | O(1). |
| LINDEX | `LINDEX key index` | O(1) point lookup. |
| LSET | `LSET key index element` | `ERR no such key`, `ERR index out of range`. |
| LRANGE | `LRANGE key start stop` | |
| LTRIM | `LTRIM key start stop` | |
| LREM | `LREM key count element` | Reads the whole list and rewrites everything after the first removed element: O(N). |
| LINSERT | `LINSERT key BEFORE\|AFTER pivot element` | O(N), like `LREM` (inserting before the first element is O(1) once the pivot is found). -1 if the pivot is missing, 0 if the key is missing. |
| LPOS | `LPOS key element [RANK rank] [COUNT num-matches] [MAXLEN len]` | |
| LMOVE | `LMOVE source destination LEFT\|RIGHT LEFT\|RIGHT` | `source` and `destination` may be the same list (rotation). |
| RPOPLPUSH | `RPOPLPUSH source destination` | |
| LMPOP | `LMPOP numkeys key [key ...] LEFT\|RIGHT [COUNT count]` | From the first list that has elements: `[key, [element ...]]`; nil if none has. `ERR numkeys should be greater than 0`, `ERR count should be greater than 0`. |
| BLPOP, BRPOP | `BLPOP key [key ...] timeout` (same for `BRPOP`) | `[key, element]` from the first list that has one; waits if every list is empty. See [Blocking commands](#blocking-commands). |
| BLMOVE | `BLMOVE source destination LEFT\|RIGHT LEFT\|RIGHT timeout` | `LMOVE` that waits for `source` to get an element. |
| BRPOPLPUSH | `BRPOPLPUSH source destination timeout` | `RPOPLPUSH` that waits. |
| BLMPOP | `BLMPOP timeout numkeys key [key ...] LEFT\|RIGHT [COUNT count]` | `LMPOP` that waits. |

## Blocking commands

`BLPOP`, `BRPOP`, `BLMOVE`, `BRPOPLPUSH`, `BLMPOP`, `BZPOPMIN`, `BZPOPMAX` and `BZMPOP` do what their
non-blocking form does when one of their keys has data. When none has, the connection waits, without holding
a server thread, until a write touches one of the keys or the timeout passes.

- **Timeout.** In seconds, with fractions (`0.5`); `0` waits without limit. `ERR timeout is not a float or out
  of range` and `ERR timeout is negative` for bad values. On timeout the reply is a nil array (nil bulk for
  `BLMOVE` and `BRPOPLPUSH`).
- **Wake-up.** Any write to a watched key wakes the waiting connection, which runs the command again: it
  takes the data, or waits again (another client took it first) until the same deadline. When several clients
  wait on the same key, each element goes to exactly one of them, but not necessarily to the one that has
  waited longest: there is no first-come-first-served order as in Redis.
- **Pipelines.** Requests sent after a blocked command wait for it, and their replies follow its reply in order.
- **Clients that leave.** A connection that closes while blocked stops waiting at once, so it never takes an
  element it would not receive.
- **Inside `MULTI`**, and wherever waiting is not possible, a blocking command answers at once (nil if there is
  no data), as in Redis.
- A key of the wrong type replies `WRONGTYPE` instead of waiting.

## Transactions

`MULTI`, `EXEC`, `DISCARD`, `WATCH` and `UNWATCH`, with Redis's semantics. See [Transactions](transactions.md).

| Command | Syntax | Notes |
|---|---|---|
| MULTI | `MULTI` | Starts queueing; each later command replies `QUEUED`. `ERR MULTI calls can not be nested`. |
| EXEC | `EXEC` | Runs the queue as one isolated block and replies an array with each command's reply. Nil if a watched key changed. `EXECABORT Transaction discarded because of previous errors.` if a command was refused while queueing. `ERR EXEC without MULTI`. |
| DISCARD | `DISCARD` | Drops the queue and the watched keys. `ERR DISCARD without MULTI`. |
| WATCH | `WATCH key [key ...]` | `EXEC` fails if any of these keys is written before it. `ERR WATCH inside MULTI is not allowed`. |
| UNWATCH | `UNWATCH` | Forgets the watched keys. `EXEC` and `DISCARD` do it too. |

## Pub/Sub

Channels, patterns and shard channels. See [Pub/Sub](pubsub.md).

| Command | Syntax | Notes |
|---|---|---|
| SUBSCRIBE | `SUBSCRIBE channel [channel ...]` | One `["subscribe", channel, count]` per channel; then `["message", channel, payload]` for each message. |
| PSUBSCRIBE | `PSUBSCRIBE pattern [pattern ...]` | Glob patterns; messages arrive as `["pmessage", pattern, channel, payload]`. |
| SSUBSCRIBE | `SSUBSCRIBE shardchannel [shardchannel ...]` | Shard channels, counted apart; messages arrive as `["smessage", channel, payload]`. |
| UNSUBSCRIBE, PUNSUBSCRIBE, SUNSUBSCRIBE | `UNSUBSCRIBE [channel ...]` (same for the others) | Without arguments, from all of that kind. |
| PUBLISH | `PUBLISH channel message` | Replies how many subscribers received it (channel and pattern subscribers). |
| SPUBLISH | `SPUBLISH shardchannel message` | To shard channel subscribers only. |
| PUBSUB | `PUBSUB CHANNELS [pattern] \| NUMSUB [channel ...] \| NUMPAT \| SHARDCHANNELS [pattern] \| SHARDNUMSUB [channel ...] \| HELP` | |

A RESP2 connection with subscriptions may only run the (un)subscribe commands, `PING` (which replies
`["pong", message]`) and `QUIT`; RESP3 connections may run anything. Messages are not sent to replicas.

## Scripting

Lua scripts and Redis 7 functions, with Redis's Lua 5.1 API. A script runs atomically, with the whole keyspace
to itself. See [Lua scripting](scripting.md).

| Command | Syntax | Notes |
|---|---|---|
| EVAL | `EVAL script numkeys [key ...] [arg ...]` | Runs (and caches) a script; `KEYS` and `ARGV` hold the keys and the arguments. |
| EVALSHA | `EVALSHA sha1 numkeys [key ...] [arg ...]` | A cached script. `NOSCRIPT No matching script. Please use EVAL.` |
| EVAL_RO, EVALSHA_RO | As `EVAL`, `EVALSHA` | The script may only read. |
| SCRIPT | `SCRIPT LOAD script \| EXISTS sha1 [sha1 ...] \| FLUSH [ASYNC\|SYNC] \| KILL \| HELP` | The cache is in memory and per server. |
| FCALL | `FCALL function numkeys [key ...] [arg ...]` | Runs a function. |
| FCALL_RO | As `FCALL` | Only for functions flagged `no-writes`. |
| FUNCTION | `FUNCTION LOAD [REPLACE] code \| DELETE library \| FLUSH [ASYNC\|SYNC] \| LIST [LIBRARYNAME pattern] [WITHCODE] \| STATS \| KILL \| HELP` | Libraries are stored and replicated. `DUMP` and `RESTORE` are not supported. |

## JSON

JSON documents follow RedisJSON. Paths are JSONPath (`$...`) or legacy (`.a.b`); a missing path means the
legacy root `.`. See [JSON](json.md) for paths, replies and errors.

| Command | Syntax | What it does |
|---|---|---|
| JSON.SET | `JSON.SET key path value [NX\|XX]` | Sets the value at a path; a new key must be created at the root. |
| JSON.GET | `JSON.GET key [INDENT s] [NEWLINE s] [SPACE s] [path ...]` | Returns the values at the paths. |
| JSON.MGET | `JSON.MGET key [key ...] path` | One path from several documents. |
| JSON.MSET | `JSON.MSET key path value [key path value ...]` | Several sets, applied key by key in order. |
| JSON.DEL | `JSON.DEL key [path]` | Deletes the values at a path (the key, for the root). |
| JSON.FORGET | `JSON.FORGET key [path]` | Alias of `JSON.DEL`. |
| JSON.TYPE | `JSON.TYPE key [path]` | JSON type of the values. |
| JSON.NUMINCRBY | `JSON.NUMINCRBY key path number` | Adds to numbers. |
| JSON.NUMMULTBY | `JSON.NUMMULTBY key path number` | Multiplies numbers. |
| JSON.STRAPPEND | `JSON.STRAPPEND key [path] value` | Appends a JSON string (quoted) to strings. |
| JSON.STRLEN | `JSON.STRLEN key [path]` | String lengths. |
| JSON.ARRAPPEND | `JSON.ARRAPPEND key path value [value ...]` | Appends to arrays. |
| JSON.ARRINSERT | `JSON.ARRINSERT key path index value [value ...]` | Inserts into arrays. |
| JSON.ARRLEN | `JSON.ARRLEN key [path]` | Array lengths. |
| JSON.ARRPOP | `JSON.ARRPOP key [path [index]]` | Removes and returns an element (the last by default). |
| JSON.ARRTRIM | `JSON.ARRTRIM key path start stop` | Keeps an inclusive range, like `LTRIM`. |
| JSON.ARRINDEX | `JSON.ARRINDEX key path value [start [stop]]` | Position of a value in arrays. |
| JSON.OBJKEYS | `JSON.OBJKEYS key [path]` | Member names of objects. |
| JSON.OBJLEN | `JSON.OBJLEN key [path]` | Member counts of objects. |
| JSON.TOGGLE | `JSON.TOGGLE key path` | Flips booleans. |
| JSON.CLEAR | `JSON.CLEAR key [path]` | Empties containers and sets numbers to 0. |
| JSON.MERGE | `JSON.MERGE key path value` | RFC 7386 merge patch. |

## Secondary indexes

Compound, ordered indexes over hashes and JSON documents. See [Secondary indexes](secondary-indexes.md).

| Command | Syntax |
|---|---|
| IDX.CREATE | `IDX.CREATE index [ON HASH\|JSON] [PREFIX count prefix [prefix ...]] SCHEMA field [AS name] TAG [SEPARATOR c]\|NUMERIC [field ...] [UNIQUE]` |
| IDX.DROP | `IDX.DROP index` |
| IDX.LIST | `IDX.LIST` |
| IDX.INFO | `IDX.INFO index` |
| IDX.QUERY | `IDX.QUERY index [field op value ...] [DESC] [LIMIT offset count] [NOCONTENT \| RETURN count field [field ...]]` |
| IDX.COUNT | `IDX.COUNT index [field op value ...]` |
| IDX.GET | `IDX.GET index value [value ...]` |

`op` is one of `=` (or `==`), `>`, `>=`, `<`, `<=`. `AS name` applies to `ON JSON` indexes, where a field is a
path. The default `LIMIT` is 10; a negative count means no limit. `IDX.QUERY`, `IDX.COUNT` and `IDX.GET`
return and count only the keys the logged-in user may read.

## Search

The RediSearch-compatible subset, with `TAG` and `NUMERIC` fields. See [Search](search.md).

| Command | Syntax |
|---|---|
| FT.CREATE | `FT.CREATE index [ON HASH\|JSON] [PREFIX count prefix ...] [SKIPINITIALSCAN] SCHEMA field [AS alias] TAG [SEPARATOR c] [CASESENSITIVE] \| NUMERIC [SORTABLE] ...` |
| FT.SEARCH | `FT.SEARCH index query [NOCONTENT] [WITHSCORES] [FILTER field min max] [RETURN count field [AS name] ...] [SORTBY field [ASC\|DESC]] [LIMIT offset count] [PARAMS count name value ...] [DIALECT n]` |
| FT.DROPINDEX | `FT.DROPINDEX index [DD]` |
| FT.INFO | `FT.INFO index` |
| FT._LIST | `FT._LIST` |
| FT.TAGVALS | `FT.TAGVALS index field` |

Options of RediSearch that only matter for full-text search are accepted and ignored. `TEXT`, `GEO` and
`VECTOR` fields are refused. `FT.SEARCH` returns and counts only the keys the logged-in user may read.

## Change feed

Every committed change, in order, read from the retained RocksDB WAL (needs `wal-retention`). See
[Change feed](change-feed.md).

| Command | Syntax | Notes |
|---|---|---|
| CDC.INFO | `CDC.INFO` | `enabled`, `wal_retention_seconds`, `wal_retention_size`, `first_sequence`, `last_sequence`, `wal_bytes`. |
| CDC.READ | `CDC.READ sequence\|$ [COUNT n] [MATCH pattern] [BLOCK milliseconds]` | `$` means changes written from now on. `COUNT` (default 100) is reached with whole batches. `BLOCK 0` waits without limit, without holding a thread. Only changes to keys the user may read are returned. |

## Replication

Read-only replicas that follow a primary over the Redis protocol. See [Replication](replication.md).

| Command | Syntax | Notes |
|---|---|---|
| REPLICAOF | `REPLICAOF host port \| REPLICAOF NO ONE` | Follows a primary, or promotes this server to primary. |
| SLAVEOF | `SLAVEOF host port \| SLAVEOF NO ONE` | Alias of `REPLICAOF`. |
| ROLE | `ROLE` | The replication role and offsets. |
| REPLCONF | `REPLCONF option value [option value ...]` | Sent by replicas to their primary. |
| WAIT | `WAIT numreplicas timeout` | Waits until that many replicas have every write committed before it (or `timeout` ms, 0: no limit); replies how many have. |
| FAILOVER | `FAILOVER [TO host port [FORCE]] [TIMEOUT ms] [ABORT]` | Hands the primary role to a replica without losing writes. |
| REPLSYNC | `REPLSYNC replication-id offset` | Sent by a replica; turns the connection into its replication stream. datadam replicas use it instead of `PSYNC`. |

## ACL

Users, per-command and per-key permissions, and the refusal log. See [Security](security.md).

| Command | Syntax | Notes |
|---|---|---|
| ACL SETUSER | `ACL SETUSER username [rule ...]` | Creates or changes a user; all rules apply or none. |
| ACL GETUSER | `ACL GETUSER username` | `flags`, `passwords` (SHA-256), `commands`, `keys`, `channels`, `selectors` (always empty). |
| ACL DELUSER | `ACL DELUSER username [username ...]` | The `default` user cannot be deleted. |
| ACL LIST | `ACL LIST` | One `user <name> <rules>` line per user. |
| ACL USERS | `ACL USERS` | |
| ACL WHOAMI | `ACL WHOAMI` | Allowed to every logged-in user. |
| ACL CAT | `ACL CAT [category]` | Categories, or the commands in one. Allowed to every logged-in user. |
| ACL LOG | `ACL LOG [count \| RESET]` | Recent refusals and failed logins, newest first (10 by default). |

## Differences from Redis at a glance

- `DBSIZE` and `INFO keyspace` are estimates; `SELECT` accepts only database 0.
- `SCAN`, `HSCAN`, `SSCAN` and `ZSCAN` return every item exactly once; cursors are server-side positions that
  any connection can use.
- `KEYS`, `SCAN`, `IDX.QUERY`, `IDX.COUNT`, `IDX.GET`, `FT.SEARCH` and `CDC.READ` return only the keys the ACL
  user may read. Redis returns every key from `KEYS` and `SCAN`.
- Hash fields and set members come back in byte order, not insertion or hash order.
- `ZRANK`, rank-based `ZRANGE`, `ZREMRANGEBYRANK`, `ZPOPMIN`/`ZPOPMAX`, `ZCOUNT`, `ZLEXCOUNT`, `LREM`,
  `LINSERT`, `LPOS`, `SPOP`, `SRANDMEMBER` and `HRANDFIELD` are O(N) or O(rank) where Redis is often
  O(log N) or O(1). Collection reads and writes of small collections are served from the object cache.
- `DEL`, `UNLINK`, `EXPIRE` and `SET` over a collection are O(1), whatever its size.
- `FLUSHALL`, `FLUSHDB` and `SAVE` always run synchronously. A backup is a directory (a RocksDB checkpoint),
  not a `dump.rdb` file.
- `COMMAND`, `COMMAND DOCS` and `COMMAND INFO` return no documentation; `HELLO` always reports `role master`.
- `INFO server` (`redis_version`) and `HELLO` (`version`) report 7.2.0, for clients that check the Redis
  version.
- Clients blocked on the same key are not served in arrival order.
- `PUBLISH` reaches the subscribers of the server it is sent to; it is not propagated to replicas.
- Inside `EXEC`, other clients wait for the block: no read or write of theirs sees part of it; see
  [Transactions](transactions.md).
- Lua scripts run on LuaJIT (Lua 5.1) with the JIT off; `struct`, `cmsgpack`, `FUNCTION DUMP` and
  `FUNCTION RESTORE` are missing, and the 5 s busy threshold is fixed; see [Lua scripting](scripting.md).

## Unsupported commands

Any command not listed on this page replies `ERR unknown command`. Notable ones:

| Area | Missing commands |
|---|---|
| Scripting and functions | `FUNCTION DUMP`, `FUNCTION RESTORE`, `SCRIPT DEBUG` |
| Pub/Sub | Keyspace notifications (`notify-keyspace-events`) |
| Sorted sets | `ZUNIONSTORE`, `ZINTERSTORE`, `ZDIFFSTORE`, `ZUNION`, `ZINTER`, `ZDIFF`, `ZINTERCARD`, `ZRANDMEMBER`, `ZRANGESTORE` |
| Hashes | Field expiration (`HEXPIRE`, `HPEXPIRE`, `HTTL`, `HPERSIST`...), `HGETDEL`, `HGETEX`, `HSETEX` |
| Strings | `LCS`, bitmaps (`SETBIT`, `GETBIT`, `BITCOUNT`, `BITPOS`, `BITOP`, `BITFIELD`) |
| Other types | Streams (`XADD`, `XREAD`...), HyperLogLog (`PFADD`, `PFCOUNT`, `PFMERGE`), geospatial (`GEOADD`, `GEOSEARCH`...) |
| Keyspace | `COPY`, `MOVE`, `SWAPDB`, `RANDOMKEY`, `DUMP`, `RESTORE`, `MIGRATE`, `SORT`, `SORT_RO` |
| Server | `LATENCY`, `MEMORY STATS`/`DOCTOR`, `DEBUG`, `BGREWRITEAOF`, `RESET`, `WAITAOF`, `MODULE`, `CLUSTER`, `READONLY`, `READWRITE`, `SYNC`, `PSYNC` |
| CLIENT | `CLIENT PAUSE`, `CLIENT UNPAUSE`, `CLIENT TRACKING`, `CLIENT CACHING`, `CLIENT HELP` |
| ACL | `ACL SAVE`, `ACL LOAD`, `ACL GENPASS`, `ACL DRYRUN`, `ACL HELP`, and selectors |
| JSON and search | `JSON.RESP`, `JSON.DEBUG`, `FT.AGGREGATE`, `FT.ALTER`, `FT.EXPLAIN`, `FT.ALIASADD` and the other alias commands |
