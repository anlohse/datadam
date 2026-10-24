# Adding a command

A step-by-step guide to adding a Redis command to datadam: where the handler goes and how it is registered, the
`Cmd` and `Reply` APIs, the metadata that drives ACLs and replica checks, how to use `Database` correctly
(transactions, cache-miss friendly reads), what is maintained for you (indexes, change feed), and how to test and
document it. It ends with a worked example and the code conventions of the project.

## Contents

- [1. Pick the file](#1-pick-the-file)
- [2. Write the handler](#2-write-the-handler)
- [3. Register it](#3-register-it)
- [4. Add its metadata](#4-add-its-metadata)
- [5. Access the keyspace](#5-access-the-keyspace)
- [6. Make reads cache-miss friendly](#6-make-reads-cache-miss-friendly)
- [7. What you get for free](#7-what-you-get-for-free)
- [8. Test it](#8-test-it)
- [9. Document it](#9-document-it)
- [Worked example: HDELIFEQ](#worked-example-hdelifeq)
- [Worked example: a deferrable read](#worked-example-a-deferrable-read)
- [Checklist](#checklist)
- [Code conventions](#code-conventions)

## 1. Pick the file

| Command family | Handler file | Registration |
|---|---|---|
| Connection, server, strings, keyspace | [redis_commands.cpp](../../src/server/redis_commands.cpp) | the initializer of `build_command_table()` |
| Hash, set, sorted set, list | [cmd_hash.cpp](../../src/server/cmd_hash.cpp), [cmd_set.cpp](../../src/server/cmd_set.cpp), [cmd_zset.cpp](../../src/server/cmd_zset.cpp), [cmd_list.cpp](../../src/server/cmd_list.cpp) | `hash_commands()`, `set_commands()`, `zset_commands()`, `list_commands()` |
| SCAN family | [cmd_scan.cpp](../../src/server/cmd_scan.cpp) | `scan_commands()` |
| JSON, IDX, FT, CDC, ACL, replication | [cmd_json.cpp](../../src/server/cmd_json.cpp), [cmd_index.cpp](../../src/server/cmd_index.cpp), [cmd_search.cpp](../../src/server/cmd_search.cpp), [cmd_cdc.cpp](../../src/server/cmd_cdc.cpp), [cmd_acl.cpp](../../src/server/cmd_acl.cpp), [cmd_repl.cpp](../../src/server/cmd_repl.cpp) | `json_commands()`, `index_commands()`, `search_commands()`, `cdc_commands()`, `acl_commands()`, `replication_commands()` |

For a new family, create `src/server/cmd_<family>.cpp`, include `"server/command.h"`, define
`CommandList <family>_commands()`, declare it in [command.h](../../src/server/command.h) next to the others, add
it to the list that `build_command_table()` merges, and add the file to `datadam_core` in
[CMakeLists.txt](../../CMakeLists.txt).

## 2. Write the handler

A handler is a free function `void cmd_<name>(Cmd& c)` (type `Handler`), normally in the file's anonymous
namespace. `Cmd` ([command.h](../../src/server/command.h)) gives it:

| Member | Type | Content |
|---|---|---|
| `c.args` | `Args` (`std::span<const std::string_view>`) | The request: `args[0]` is the command name as the client sent it, then the arguments. The views point into the connection's input buffer and are valid only during the call |
| `c.r` | `Reply&` | Where the reply goes |
| `c.db()` | `Database&` | The keyspace |
| `c.ctx` | `ServerContext&` | Settings (`ctx.settings()`, `ctx.config` for fixed ones), stats, ACL, cursors, replication state, read pool |
| `c.client` | `ClientState&` | The connection: protocol version, user, name, flags for retries and blocking |

The `Reply` API ([resp.h](../../src/server/resp.h)) appends one RESP value per call; containers announce their
size first and are followed by that many values:

| Call | RESP2 | RESP3 |
|---|---|---|
| `ok()`, `simple(s)` | `+OK`, `+s` | same |
| `error(msg)` | `-msg` (CR and LF in `msg` become spaces) | same |
| `integer(v)` | `:v` | same |
| `bulk(s)` | `$len` + bytes | same |
| `null()` | `$-1` | `_` |
| `null_array()` | `*-1` | `_` |
| `array(n)` | `*n` | same |
| `map(n)` | `*2n` (flattened) | `%n` |
| `set(n)` | `*n` | `~n` |
| `double_value(v)` | bulk string (`format_double`) | `,v` |

`c.r.proto()` is 2 or 3 (switched by `HELLO`), for replies whose shape differs more than these calls handle
(`HRANDFIELD ... WITHVALUES` sends pairs as nested arrays in RESP3 and flat in RESP2). A handler sends exactly
one top-level value. `Reply` returns `void`, so handlers end early with `return c.r.error(...);`.

Error messages start with the Redis error code: `ERR ...`, `WRONGTYPE ...`, and so on. Reuse the constants in
`command.h` (`kWrongType`, `kNotInteger`, `kNotFloat`, `kSyntax`, `kMustBePositive`) and Redis' own wording
where clients may match it.

Before the handler runs, `execute_redis_command` has already checked authentication, arity, ACL permissions,
`READONLY` and `LOADING` (see [step 4](#4-add-its-metadata)). It also catches exceptions: `IndexConstraintError`
becomes its message, `StorageError` becomes `ERR storage error: ...`, any other `std::exception` becomes
`ERR internal error: ...`. A handler does not need its own `try`.

## 3. Register it

Add a `{name, CommandSpec}` pair to the file's `CommandList` (or to `build_command_table()`):

```cpp
CommandList hash_commands() {
  return {
      {"hset", {cmd_hset, -4}},
      {"hget", {cmd_hget, 3}},
      // ...
  };
}
```

- The name is lower case; lookups lower-case the request's name first.
- **Arity** counts the command name: a positive arity is the exact number of arguments (`{"get", {cmd_get, 2}}`
  for `GET key`), a negative one the minimum (`{"set", {cmd_set, -3}}` for `SET key value [options...]`). A wrong
  count is answered with `ERR wrong number of arguments for '<name>' command` before the handler runs. Finer
  checks (pairs of arguments, option values) are the handler's job.
- `CommandSpec::categories`, `keys` and `id` are filled in by `build_command_table()` from the metadata table; do
  not set them here. Ids are dense, assigned in name order; per-user command bitmaps have room for
  `kMaxAclCommands` (512) commands ([acl.h](../../src/server/acl.h)), and there are 169 today.

## 4. Add its metadata

Every command needs an entry in `kEntries` in [command_meta.cpp](../../src/server/command_meta.cpp): its ACL
categories and which arguments are keys.

```cpp
{"hget", {R | HASH | FAST, {kRead1, kNone}}},
{"smove", {W | SET | FAST, {kWrite1to2, kNone}}},
{"sinterstore", {W | SET | SLOW, {kWrite1, kRead2toEnd}}},
{"sintercard", {R | SET | SLOW, {kReadNumkeys, kNone}}},
```

**Categories** are the `kCat*` bits of [command.h](../../src/server/command.h), abbreviated in the file: `R`
(read), `W` (write), `KS` (keyspace), `STR`, `HASH`, `SET`, `ZSET`, `LIST`, `JSON`, `IDX` (index), `ADMIN`,
`DANGER` (dangerous), `CONN` (connection), `FAST`, `SLOW`. Follow what Redis declares for a similar command, so
ACL rules written for Redis mean the same here.

**Key ranges** (`KeyRange`, two per command) say which arguments are keys and with which access (`kKeyRead`,
`kKeyWrite`): arguments `first` to `last` every `step` (a negative `last` counts from the end), or, with
`numkeys_at`, the number of keys is read from that argument. The file predefines the common ones: `kRead1`,
`kWrite1`, `kReadAll`, `kWriteAll`, `kWrite1to2`, `kWritePairs` (MSET), `kRead2toEnd`, `kReadNumkeys`, `kNone`.
`for_each_key(spec, args, f)` walks them.

The metadata is used for:

- **ACL** (`acl_check` in [acl.cpp](../../src/server/acl.cpp)): category and command rules, and the key patterns
  of the user for every key argument with its access. If the command has subcommands that ACL rules should be
  able to name (`+config|get`), add it to `has_subcommands()` in `acl.cpp`.
- **Read-only replicas**: a command with `W` gets `READONLY You can't write against a read only replica.` on a
  replica.
- **Full sync**: a command with `R` or `W` gets `LOADING ...` while a replica loads its primary's data.
- **Prefetch**: for deferrable reads, `uncached_read_keys` uses the key ranges to load the keys of pipelined
  requests (step 6).

A command without metadata gets `kCatAdmin | kCatDangerous` and no key ranges: only users allowed those
categories (or the command by name) can run it, their key patterns are not checked for it, and it is not refused
on a replica or during a full sync. On a replica its writes would then be dropped by `Database` (a read-only
database writes nothing to RocksDB) while the cache is still updated. So a missing entry is a bug.

Commands that return keys found by other means than their arguments (`KEYS`, `SCAN`, `IDX.QUERY`, `CDC.READ`)
must filter them with `may_read_key(c, key)`; `reads_all_keys(c)` tells when no filtering is needed:

```cpp
auto keys = c.db().keys(c.args[1]);
if (!reads_all_keys(c)) std::erase_if(keys, [&](const std::string& k) { return !may_read_key(c, k); });
```

## 5. Access the keyspace

Always go through `Database` ([database.h](../../src/db/database.h)); never call `RocksStorage` for user data
from a command. Which API to use:

| Need | API |
|---|---|
| Read a string or JSON record | `c.db().read(key, [&](const RecordView& v) {...})` returns false if the key is missing. On a cache hit the callback runs under the cache shard mutex, straight from pooled memory: write the reply or copy what you need, but do not call the cache or `Database` from it, and keep heavy work (parsing JSON) outside |
| Read a copy | `c.db().get(key)` (`std::optional<Record>`), `c.db().exists(key)` |
| Read-modify-write a string or JSON | `c.db().update(key, [&](std::optional<Record>& rec) { ...; return Database::Op::Put; })` with `Op::Keep`, `Put` (new CAS), `PutKeepCas` (TTL-only changes) or `Delete` |
| Blind write / delete | `c.db().put(key, Record)`, `c.db().del(key)` |
| Read or change a collection | `with_collection(c, key, type, [&](Database::KeyTxn& t) {...})` (replies `WRONGTYPE` itself), or `c.db().transact(key, f)` directly |
| Read a collection | `read_collection(c, key, type, need_elements, f)`: served from the cache without the key lock when possible, else a transaction |
| Several keys atomically | `c.db().transact_many(keys, [&](Database::KeyTxns& txns) {...})` |
| Scan RocksDB directly | `c.db().commit_active_batch()` first, then `storage().cursor(...)` |

`KeyTxn` offers `exists()`, `wrong_type(t)`, `record()`, `create(t)` (make it an empty collection of type `t`
unless it already is), `reset(t)`, `remove()`, element access `get(sub)`, `put(sub, value)`, `del(sub)`,
`scan(from, to, reverse, fn)`, and `meta()` / `size()`. The handler keeps `meta().size` (and `meta().head` for
lists) right itself; a collection whose size drops to 0 is deleted when the transaction finishes. Element
sub-key layouts per type are at the top of each `cmd_*.cpp` and in [storage-format.md](storage-format.md).

Rules that come from [concurrency.md](concurrency.md):

- **The transaction callback may be rolled back** (an exception, a UNIQUE clash found after it ran). It must not
  write the reply: collect results in local variables and reply after `with_collection` / `transact` returns.
- **Do not suspend.** Handlers are synchronous; a command that must wait uses `ClientState` flags as
  `CDC.READ BLOCK` does (`retry_later`, `wait_for_writes`) and is run again.
- **Validate arguments before touching the database**, so an error leaves nothing half done.

## 6. Make reads cache-miss friendly

A plain read that misses the cache can be deferred to the read pool instead of reading the disk on an I/O thread
(see [concurrency.md](concurrency.md#the-read-pool)). A command qualifies only if:

- it reads keys only through `Database::read`, `get` or `exists`;
- it has no side effect at all, so running it again from the start is harmless (its partial reply is rewound);
- its metadata key ranges list exactly the keys it reads (used for prefetching).

Then add its name to the list in `rerunnable()` in [redis_commands.cpp](../../src/server/redis_commands.cpp).
`execute_redis_command` runs it inside a `Database::NonBlockingReads` scope, a miss throws `Database::WouldBlock`
(caught for you), and the connection runs the command again once the key is cached. Commands that go through
`transact` or `peek` (collection reads), and anything that writes, are not deferrable.

## 7. What you get for free

As long as the command writes through `Database`:

- **Atomicity and group commit.** Everything a command writes reaches RocksDB in one batch with the rest of the
  pipeline round, and the reply is sent only after that batch is written.
- **Secondary indexes.** `KeyTxn::put`, `del`, `create`, `reset` and `remove` record the hash's index tuples
  before the first change, and `TxnRun::finish` writes the index changes (and checks UNIQUE constraints) in the
  same batch. `Database::update` and `put` do the same for JSON documents, and drop or refresh hash index entries
  when a hash is deleted, replaced or gets a new TTL. See [indexes-internals.md](indexes-internals.md).
- **Change feed.** With the WAL retained, every batch carries a change note (commit time, and the type of each
  collection version whose elements it writes, recorded by `KeyTxn`), so `CDC.READ` decodes the command's writes
  without any code in the handler. See [replication-and-cdc-internals.md](replication-and-cdc-internals.md).
- **Replication.** Replicas receive the RocksDB write batches themselves, so a new command needs no replication
  code.
- **Expiration.** Expired keys look missing to `read`, `get`, `update` and `KeyTxn`.
- **The object cache.** Records (and small collections' elements) are cached and invalidated by `Database`.

## 8. Test it

**Unit test.** Add a `TEST_F` to the suite closest to the command:
[collections_test.cpp](../../tests/collections_test.cpp) for collections,
[database_test.cpp](../../tests/database_test.cpp) for `Database` behavior,
[admin_test.cpp](../../tests/admin_test.cpp) for server and SCAN commands,
[json_test.cpp](../../tests/json_test.cpp),
[index_test.cpp](../../tests/index_test.cpp), [search_test.cpp](../../tests/search_test.cpp),
[cdc_test.cpp](../../tests/cdc_test.cpp), [acl_test.cpp](../../tests/acl_test.cpp). These fixtures open a fresh
RocksDB directory, a small cache, a `Database` and a `ServerContext`, and run commands through
`execute_redis_command` with a RESP2 `Reply`:

```cpp
// The fixture's helper (collections_test.cpp): the raw RESP reply of one command.
std::string run(std::vector<std::string> args) {
  std::vector<std::string_view> views(args.begin(), args.end());
  std::string out;
  Reply reply(out, 2);
  execute_redis_command(*ctx_, client_, views, reply);
  return out;
}
```

Cover the normal case, the error replies (wrong type, bad arguments), and, if relevant, a restart (`close()`
then `open()` in `CollectionsTest`) and an ACL check with a restricted user (`AclTest`).

**redis-cli suite.** Add lines to the right section of the `commands()` function in
[tests/redis_cli_test.sh](../../tests/redis_cli_test.sh), with the helpers `t` (exact output), `tc`
(substring), `tx` (regex). This runs the command through the real `redis-cli`, over plain TCP and over TLS, in
`tests/docker_redis_cli.sh`. If the command stores data that must survive a restart, also extend `write_phase`
and `verify_phase`.

See [building-and-testing.md](building-and-testing.md) for running them.

## 9. Document it

- [docs/commands.md](../commands.md): syntax, reply, differences from Redis. Also the topic page if there is one
  ([json.md](../json.md), [secondary-indexes.md](../secondary-indexes.md), [search.md](../search.md),
  [change-feed.md](../change-feed.md)).
- The header comment of the `cmd_*.cpp` file lists the syntax of its commands; keep it current.
- The "Supported commands" list in [README.md](../../README.md).

## Worked example: HDELIFEQ

> **Example only.** `HDELIFEQ` does not exist in datadam or in Redis. It shows the real APIs on a small write
> command: `HDELIFEQ key field value` removes `field` from the hash at `key` if it holds exactly `value`, and
> replies 1 if it did, 0 otherwise.

**Handler**, in the anonymous namespace of [cmd_hash.cpp](../../src/server/cmd_hash.cpp), where `Txn` and `kHash`
are already defined:

```cpp
// HDELIFEQ key field value: removes the field only if it holds exactly this value.
void cmd_hdelifeq(Cmd& c) {
  bool removed = false;
  if (!with_collection(c, c.args[1], kHash, [&](Txn& t) {
        if (!t.exists()) return;
        auto v = t.get(c.args[2]);
        if (!v || *v != c.args[3]) return;
        t.del(c.args[2]);
        --t.meta().size;  // the handler keeps the size; at 0 the hash is deleted
        removed = true;
      })) {
    return;  // with_collection replied WRONGTYPE
  }
  c.r.integer(removed ? 1 : 0);  // the reply comes after the transaction
}
```

**Registration**, in `hash_commands()`: exactly four arguments, the name included.

```cpp
{"hdelifeq", {cmd_hdelifeq, 4}},
```

**Metadata**, in the hash section of `kEntries` in [command_meta.cpp](../../src/server/command_meta.cpp): a fast
hash write whose first argument is a key it writes.

```cpp
{"hdelifeq", {W | HASH | FAST, {kWrite1, kNone}}},
```

With that, a user with `-@write` or without write access to the key gets `NOPERM`, a replica answers `READONLY`,
an index on that field loses the hash's entry in the same batch, and `CDC.READ` shows an `hdel` change.

**Unit test**, in [collections_test.cpp](../../tests/collections_test.cpp), with the file's `integer` and
`bulk` helpers:

```cpp
TEST_F(CollectionsTest, HdelifeqRemovesOnlyAMatchingValue) {
  EXPECT_EQ(run({"HSET", "h", "a", "1", "b", "2"}), integer(2));
  EXPECT_EQ(run({"HDELIFEQ", "h", "a", "9"}), integer(0));
  EXPECT_EQ(run({"HDELIFEQ", "h", "a", "1"}), integer(1));
  EXPECT_EQ(run({"HGET", "h", "a"}), "$-1\r\n");
  EXPECT_EQ(run({"HDELIFEQ", "h", "b", "2"}), integer(1));
  EXPECT_EQ(run({"EXISTS", "h"}), integer(0));  // the empty hash is gone
  run({"SET", "s", "x"});
  EXPECT_TRUE(run({"HDELIFEQ", "s", "a", "1"}).starts_with("-WRONGTYPE"));
}
```

**redis-cli lines**, in the `hash` section of [redis_cli_test.sh](../../tests/redis_cli_test.sh) (the section
already has a string key `str`):

```bash
  t  "HDELIFEQ setup"           "1" HSET hdif f v
  t  "HDELIFEQ mismatch"        "0" HDELIFEQ hdif f other
  t  "HDELIFEQ match"           "1" HDELIFEQ hdif f v
  tc "HDELIFEQ wrong type"      "WRONGTYPE" HDELIFEQ str f v
```

## Worked example: a deferrable read

> **Example only.** `GETPREFIX key n`, the first `n` bytes of a string, is not a datadam or Redis command.

In [redis_commands.cpp](../../src/server/redis_commands.cpp), next to `cmd_strlen` (the file's `is_string`
helper is in scope):

```cpp
// GETPREFIX key n
void cmd_getprefix(Cmd& c) {
  auto n = parse_int64(c.args[2]);
  if (!n || *n < 0) return c.r.error(kNotInteger);  // validate before reading
  bool found = c.db().read(c.args[1], [&](const RecordView& v) {
    if (!is_string(v)) return c.r.error(kWrongType);
    c.r.bulk(v.value.substr(0, static_cast<size_t>(*n)));  // straight from the cache
  });
  if (!found) c.r.null();
}
```

Then `{"getprefix", {cmd_getprefix, 3}}` in `build_command_table()`, `{"getprefix", {R | STR | FAST, {kRead1,
kNone}}}` in `kEntries`, and `"getprefix"` in the name list of `rerunnable()`. It reads only through
`Database::read`, has no side effect, and its only key is argument 1, so a miss is loaded by the read pool and the
command runs again.

## Checklist

- [ ] Handler `cmd_<name>(Cmd&)` in the right file, arguments validated before any database access.
- [ ] Registered with the right arity.
- [ ] Metadata entry with categories (`R` or `W` at least, for data commands) and key ranges.
- [ ] Keyspace accessed only through `Database`; transaction callbacks do not write replies.
- [ ] Added to `rerunnable()` if it is a plain, side-effect-free read through `read` / `get` / `exists`.
- [ ] Keys found by other means filtered with `may_read_key`.
- [ ] RESP2 and RESP3 replies checked (`HELLO 3` in redis-cli, or a `Reply` with protocol 3).
- [ ] Unit test and redis-cli lines.
- [ ] `docs/commands.md` (and topic page), the file's header comment, README list.

## Code conventions

What the existing code does, and new code should follow:

- **Names.** Everything is in namespace `datadam`; file-local helpers in an anonymous namespace. Types are
  `PascalCase` (`KeyTxn`, `RecordView`), functions and variables `snake_case` (`load_into_cache`), constants
  `kPascalCase` (`kMaxBatchWrites`, `kWrongType`); data members of classes end with `_` (`cache_`, `locks_`),
  fields of plain structs do not (`PendingBatch::held`, `ClientState::proto`). Enumerations are `enum class`
  except the ACL bit sets (`AclCategory`, `KeyAccess`). Command handlers are `cmd_<name>`, shared variants
  `<name>_generic` (`setex_generic`, `expire_generic`).
- **Files.** `#pragma once`; includes are relative to `src/` (`"db/database.h"`); a `.cpp` includes its own
  header first, then standard headers, then project headers. Two-space indentation, lines up to about 120
  columns, `if (x) return ...;` on one line when short.
- **Comments.** Full sentences that explain why, or what a declaration means, above it; no Doxygen markup. Each
  `cmd_*.cpp` starts with a comment describing the data layout or the syntax of its commands; handlers with
  options have their syntax in a comment (`// SET key value [NX|XX] [GET] ...`).
- **Error replies.** Start with the Redis code (`ERR`, `WRONGTYPE`, `NOPERM`, `NOAUTH`, `WRONGPASS`, `READONLY`,
  `LOADING`, `NOPROTO`), match Redis' wording where clients rely on it, and are built with `std::format`. Option
  keywords are compared after `to_lower`; numbers are parsed with the strict `parse_int64` / `parse_uint64` /
  `parse_double` / `parse_long_double`, and doubles printed with `format_double`.
- **Exceptions.** Used for failures, not normal control flow: `StorageError` (RocksDB), `IndexConstraintError`
  (UNIQUE clash), `std::logic_error` (API misuse). `Database::WouldBlock` is the one control-flow exception and
  deliberately does not derive from `std::exception`. No exception crosses a command boundary
  (`execute_redis_command` turns them into error replies), a connection coroutine (`serve_redis` and
  `memcache_session` catch `std::exception` and close the connection), a background thread loop (the maintenance
  loop, read pool threads and backfills catch and log), or a destructor (`~WriteBatchScope` logs a failed
  commit).
- **Ownership and threads.** `std::unique_ptr` for owned objects, `std::shared_ptr` only for shared lifetimes
  (ACL users, index registry entries, the TLS context). Counters are atomics with `std::memory_order_relaxed`.
  Classes that must not be copied delete their copy operations; thread members are declared last so they start
  after the rest of the object exists (`Maintenance`, `ReadPool`).
- **Logging.** `log_debug` / `log_info` / `log_warn` / `log_error` with `std::format` syntax; nothing on the hot
  path above `debug`.
