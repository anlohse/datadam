# Lua scripting

`EVAL`, `EVALSHA`, `SCRIPT`, and Redis 7 functions (`FUNCTION`, `FCALL`) work as in Redis, with the same Lua
5.1 dialect and the same `redis` API, so scripts written for Redis run unchanged. A script runs atomically: no
other client reads or writes anything while it runs, whichever keys it touches.

## Contents

- [EVAL and EVALSHA](#eval-and-evalsha)
- [The script cache](#the-script-cache)
- [The redis API](#the-redis-api)
- [Replies: Lua to Redis and back](#replies-lua-to-redis-and-back)
- [Errors](#errors)
- [Read-only scripts](#read-only-scripts)
- [Functions](#functions)
- [Atomicity, long scripts and SCRIPT KILL](#atomicity-long-scripts-and-script-kill)
- [The sandbox](#the-sandbox)
- [Permissions](#permissions)
- [Replication](#replication)
- [Differences from Redis](#differences-from-redis)

## EVAL and EVALSHA

```text
> EVAL "return redis.call('INCRBY', KEYS[1], ARGV[1])" 1 counter 5
(integer) 5
> SCRIPT LOAD "return redis.call('GET', KEYS[1])"
"d3c21d0c2b9ca22f82737626a27bcaf5d288f99f"
> EVALSHA d3c21d0c2b9ca22f82737626a27bcaf5d288f99f 1 counter
"5"
```

`EVAL script numkeys key [key ...] arg [arg ...]`: the first `numkeys` arguments after `numkeys` are the
script's `KEYS` table, the rest its `ARGV` table (both strings, 1-based). Pass the key names a script uses in
`KEYS`: ACL key patterns are checked against them before the script runs (see [Permissions](#permissions)).

`EVALSHA sha1 numkeys ...` runs a cached script by the SHA1 of its text (in lower or upper case).
`EVAL_RO` and `EVALSHA_RO` run a script that may only read (see [Read-only scripts](#read-only-scripts)).

## The script cache

Every `EVAL` caches its script, and `SCRIPT LOAD` caches one without running it. The cache is in memory: a
restarted server, or a replica, starts with an empty one, and `EVALSHA` replies
`NOSCRIPT No matching script. Please use EVAL.` for a script it does not have; clients such as redis-py and
Lettuce fall back to `EVAL` on that error.

As in Redis 7.4, scripts that only `EVAL` brought in are evicted beyond 500, least recently used first (a
`EVAL` or `EVALSHA` of a script counts as a use), so an application that builds a new script text for every call
cannot fill the memory. Scripts loaded with `SCRIPT LOAD` are never evicted.

| Command | Effect |
|---|---|
| `SCRIPT LOAD script` | Compiles and caches it; replies its SHA1. A syntax error is replied and nothing is cached. |
| `SCRIPT EXISTS sha1 [sha1 ...]` | `1` or `0` for each. |
| `SCRIPT FLUSH [ASYNC \| SYNC]` | Empties the cache (always at once). |
| `SCRIPT KILL` | Stops the running script if it has not written anything (see [below](#atomicity-long-scripts-and-script-kill)). |

## The redis API

| Function | What it does |
|---|---|
| `redis.call(command, arg, ...)` | Runs a command and returns its reply as a Lua value. An error reply raises a Lua error that ends the script (unless caught with `pcall`). Arguments must be strings or numbers. |
| `redis.pcall(command, arg, ...)` | The same, but an error reply is returned as a table `{err = "..."}` instead of raised. |
| `redis.error_reply(msg)` / `redis.status_reply(msg)` | `{err = msg}` / `{ok = msg}`, to return an error or a status reply. |
| `redis.sha1hex(s)` | The SHA1 of `s`, in hex. |
| `redis.log(level, msg, ...)` | Writes to the server log; `redis.LOG_DEBUG`, `LOG_VERBOSE`, `LOG_NOTICE`, `LOG_WARNING`. |
| `redis.setresp(2 \| 3)` | The protocol `redis.call` replies in for the rest of this script (RESP2 by default). |
| `redis.set_repl`, `redis.replicate_commands`, `redis.breakpoint`, `redis.debug` | Accepted and do nothing (`replicate_commands` returns true): replication always sends the effects. |
| `redis.REDIS_VERSION`, `redis.REDIS_VERSION_NUM` | `"7.2.0"` and `0x070200`. |

The libraries are those Redis gives scripts: `base` (without the functions listed under
[The sandbox](#the-sandbox)), `table`, `string`, `math`, `os.clock`, `bit`, and `cjson` (`cjson.encode`,
`cjson.decode`, `cjson.null`). `struct` and `cmsgpack` are not there.

## Replies: Lua to Redis and back

What `redis.call` returns:

| Command reply | Lua value |
|---|---|
| Integer | Number |
| Bulk string | String |
| Nil bulk, nil array | `false` (RESP3 null: `nil`) |
| Array | Table (array) |
| Status (`OK`) | `{ok = "OK"}` |
| Error | `{err = "..."}` (with `redis.pcall`; `redis.call` raises it) |
| RESP3 map / set / double / boolean / big number | `{map = {...}}` / `{set = {member = true}}` / `{double = n}` / boolean / `{big_number = "..."}` |

What a script's return value becomes:

| Lua value | Reply |
|---|---|
| Number | Integer (truncated: `return 3.99` replies `3`) |
| String | Bulk string |
| `true` | Integer `1` |
| `false`, `nil` | Nil |
| Table with `err` / `ok` | Error / status reply |
| Table with `double`, `map` or `set` | Double, map or set (RESP3; arrays in RESP2) |
| Other table | Array of its elements 1, 2, ... up to the first `nil` |

## Errors

| Situation | Reply |
|---|---|
| The script does not compile | `ERR Error compiling script (new function): user_script:1: ...` |
| A Lua error (`error("x")`, calling a nil value...) | `ERR user_script:1: x script: <sha1>` |
| `redis.call` gets an error reply | That error, e.g. `WRONGTYPE Operation against a key holding the wrong kind of value script: <sha1>` |
| `redis.call` with an unknown command, or one scripts may not call | `ERR Unknown Redis command called from script` / `ERR This Redis command is not allowed from script` |
| A command the ACL user may not run | Its `NOPERM ...` error, raised like any other |
| Reading or creating a global variable | `ERR user_script:1: Script attempted to access nonexistent global variable 'x'` / `... to create global variable 'x'` |
| `numkeys` too large or negative | `ERR Number of keys can't be greater than number of args` / `ERR Number of keys can't be negative` |
| A script stopped by `SCRIPT KILL` | `ERR user_script:1: Script killed by user with SCRIPT KILL... script: <sha1>` |

As in Redis, a script that fails after writing keeps the writes it made.

Scripts may not call: `MULTI`, `EXEC`, `DISCARD`, `WATCH`, `UNWATCH`, the `SUBSCRIBE` family, `MONITOR`,
`EVAL` and the other scripting commands, `AUTH`, `HELLO`, `QUIT`, `RESET`, `CLIENT`, `WAIT`, `SHUTDOWN`,
`FAILOVER`, `REPLICAOF` / `SLAVEOF`, the replication handshake commands and `CDC.READ`. Blocking commands such
as `BLPOP` answer at once (nil when there is no data), as inside `MULTI`.

## Read-only scripts

A script can declare that it only reads with a shebang line:

```lua
#!lua flags=no-writes
return redis.call('GET', KEYS[1])
```

A read-only script (that one, or any script run with `EVAL_RO` / `EVALSHA_RO`, or a function run with
`FCALL_RO`) gets `ERR Write commands are not allowed from read-only scripts.` from `redis.call` with a
command that writes. The other flags Redis defines (`allow-oom`, `allow-stale`, `no-cluster`,
`allow-cross-slot-keys`) are accepted and change nothing; an unknown flag is an error.

## Functions

A library is Lua code that starts with `#!lua name=<library>` and registers functions:

```lua
#!lua name=counters

local function bump(keys, args)
  return redis.call('INCRBY', keys[1], args[1] or 1)
end

redis.register_function('bump', bump)
redis.register_function{
  function_name = 'peek',
  callback = function(keys) return redis.call('GET', keys[1]) end,
  flags = {'no-writes'},
}
```

```text
$ redis-cli -x FUNCTION LOAD < counters.lua
"counters"
> FCALL bump 1 visits 10
(integer) 10
> FCALL_RO peek 1 visits
"10"
```

A function gets the keys and arguments as its two parameters (`keys`, `args`), not as `KEYS` and `ARGV`.

| Command | Effect |
|---|---|
| `FUNCTION LOAD [REPLACE] code` | Loads a library; replies its name. Without `REPLACE`, a library of that name is an error. Function names are unique across libraries, and are made of letters, digits and `_`. |
| `FCALL function numkeys key ... arg ...` | Runs a function. |
| `FCALL_RO function numkeys ...` | Runs a function flagged `no-writes`; any other replies `ERR Can not execute a script with write flag using *_ro command.` |
| `FUNCTION LIST [LIBRARYNAME pattern] [WITHCODE]` | Each library: `library_name`, `engine` (`LUA`), `functions` (`name`, `description`, `flags`), and `library_code` with `WITHCODE`. |
| `FUNCTION DELETE library` | Removes a library and its functions. |
| `FUNCTION FLUSH [ASYNC \| SYNC]` | Removes every library. |
| `FUNCTION STATS` | The running script, if any, and the number of libraries and functions. |
| `FUNCTION KILL` | As `SCRIPT KILL`. |

While a library loads, its code may only call `redis.register_function` and `redis.log`: calling
`redis.call` at load time is an error. Each library has its own global table (it may define helper globals);
the functions see the full `redis` API when they run.

Libraries are stored in the database. They survive restarts and `FLUSHALL`, are in backups, and are
replicated: a replica has the primary's libraries and can run them (a function that writes fails there with
`READONLY`). `FUNCTION LOAD`, `DELETE` and `FLUSH` are refused on a replica.

## Atomicity, long scripts and SCRIPT KILL

A script (or function) runs with the whole keyspace to itself: it waits until no other command is reading or
writing, then runs while everyone else waits, and its writes reach RocksDB and the cache together when it
ends. Other clients see all of a script's writes or none of them. While its writes are being committed, only
the keys its commands named stay locked, so other clients go on with other keys (a script that runs `KEYS`,
`SCAN`, `FLUSHALL` or an index query keeps the whole keyspace until the commit ends). A script inside `MULTI` runs as part of the
`EXEC` block.

Scripts run one at a time. Keep them short: while one runs, the whole server waits for it.

- A command that reads or writes keys and arrives after a script has run for 1 ms waits for it off the I/O
  threads, and runs as soon as the script ends. One that comes in sooner waits on its thread (for a
  short script, that is quicker).
- After 5 seconds, other commands are answered
  `BUSY Redis is busy running a script. You can only call SCRIPT KILL, FUNCTION KILL or SHUTDOWN NOSAVE.`
  (`AUTH`, `HELLO`, `PING`, `QUIT` and `FUNCTION STATS` are answered too).
- `SCRIPT KILL` (or `FUNCTION KILL`) stops a running script that has not written anything yet; it ends with
  an error. A script that wrote cannot be stopped without breaking its atomicity:
  `UNKILLABLE Sorry the script already executed write commands against the dataset...`. `SHUTDOWN NOSAVE`
  stops the server. With no script running, the reply is `NOTBUSY No scripts in execution right now.`

The check for `SCRIPT KILL` runs every 100,000 Lua instructions, so a script stuck inside one long command
call (a huge `SORT`, a `KEYS *` on a big database) stops only after that call returns.

## The sandbox

Scripts cannot reach files, processes, the network, or the server's memory:

- Not available: `io`, `os` (except `os.clock`), `package`, `require`, `module`, `dofile`, `loadfile`, `load`,
  `loadstring` (which accept bytecode), `debug`, `print`, `jit` and LuaJIT's `ffi`.
- Globals cannot be created or read when they do not exist; use `local` variables. `KEYS` and `ARGV` are
  set for each script.
- Scripts share one Lua state, as in Redis: a script must not rely on what an earlier one left, and since
  globals cannot be created, it cannot leave anything but changes to the standard tables.

The interpreter is LuaJIT with its JIT compiler turned off (so `SCRIPT KILL` works), which runs Lua 5.1
code as Redis does.

## Permissions

`EVAL`, `EVALSHA`, `FCALL`, `SCRIPT` and their variants are in the `@scripting` category (`FUNCTION` is also
`@dangerous`). The keys in `KEYS` are checked against the user's key patterns when the script is called, and
every command a script runs is checked again, as the same user, with its own keys: a user who may run `EVAL`
and `GET` but not `SET` gets `NOPERM` from `redis.call('SET', ...)`. See [Security](security.md).

## Replication

A replica receives the effects of a script (the keys it wrote), never the script, so scripts that use
randomness or the time replicate correctly. Function libraries replicate as stored data (see
[Functions](#functions)); the script cache does not.

## Differences from Redis

- `EVAL` on a replica runs, and fails with `READONLY` at its first write, as in Redis 7 without
  `flags=no-writes`.
- `FUNCTION DUMP` and `FUNCTION RESTORE` are not supported.
- `struct` and `cmsgpack` are missing; `cjson` is datadam's own (built on its JSON code): it encodes tables
  whose keys are exactly 1..n as arrays and others as objects (an empty table as `{}`), and keys of objects in
  no particular order.
- The busy threshold (5 s) is fixed; there is no `busy-reply-threshold` setting.
- Scripts are not run in a separate thread: the I/O thread that received the `EVAL` runs it.
