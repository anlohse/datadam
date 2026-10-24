# Secondary indexes (IDX.*)

`IDX.*` indexes let you query hashes, or JSON documents, by the values of their fields, without maintaining index
keys by hand. An index is a compound ordered index, like a multi-column B-tree index in a relational database,
and is updated in the same write batch as the data it covers.

## Contents

- [Quick example](#quick-example)
- [Model](#model)
- [IDX.CREATE](#idxcreate)
- [Field values](#field-values)
- [Querying: IDX.QUERY and IDX.COUNT](#querying-idxquery-and-idxcount)
- [IDX.GET](#idxget)
- [IDX.INFO, IDX.LIST and IDX.DROP](#idxinfo-idxlist-and-idxdrop)
- [Consistency](#consistency)
- [UNIQUE](#unique)
- [Backfill and index states](#backfill-and-index-states)
- [Permissions](#permissions)
- [Costs](#costs)
- [IDX or FT?](#idx-or-ft)

## Quick example

```text
IDX.CREATE users PREFIX 1 user: SCHEMA city TAG age NUMERIC        # compound index (city, age)
IDX.CREATE emails PREFIX 1 user: SCHEMA email TAG UNIQUE

HSET user:1 name ana city SP age 30 email ana@x                     # the indexes are maintained on write
IDX.QUERY users city = SP age >= 18 age < 30 LIMIT 0 10             # key + fields of each hash
IDX.QUERY users city = SP name = ana RETURN 1 email                 # name filters what the index found
IDX.QUERY users city = SP DESC NOCONTENT                            # keys only, in descending index order
IDX.COUNT users city = SP
IDX.GET emails ana@x                                                # exact lookup, one value per field
IDX.INFO users
IDX.LIST
IDX.DROP users
```

The same over JSON documents, with paths in place of hash fields:

```text
IDX.CREATE users ON JSON PREFIX 1 user: SCHEMA $.address.city AS city TAG $.age AS age NUMERIC
IDX.CREATE emails ON JSON PREFIX 1 acct: SCHEMA $.email TAG UNIQUE     # no AS: the name is the path

IDX.QUERY users city = SP age > 35                  # key + ["$", document]
IDX.QUERY users city = SP $.name = ana NOCONTENT    # conditions on other paths filter the result
IDX.QUERY users city = SP RETURN 2 age $.name
```

## Model

- **Coverage.** An index covers the hashes (or, with `ON JSON`, the JSON documents) whose keys start with one of
  its prefixes, or every key when no `PREFIX` is given. Keys of other types under the prefixes are ignored.
- **Fields.** An index has 1 to 8 fields, each `TAG` (exact text) or `NUMERIC` (a number).
- **Entries.** For each covered key, the index stores one entry per tuple of field values, ordered by the first
  field, then the second, and so on, then by key. `NUMERIC` values sort numerically and `TAG` values byte by
  byte.
- **All fields required.** A key that lacks one of the fields, or whose `NUMERIC` field is not a number, is not
  in the index.
- **Query plan.** Equality conditions on the leading fields, plus range conditions (`>`, `>=`, `<`, `<=`) on
  the next field, select one contiguous slice of the index, read with a single sequential scan. Every other
  condition, on an index field or on any other field, filters the keys found in that slice.
- **Order.** Results come in index order; `DESC` reverses it.

With `SCHEMA city TAG age NUMERIC`:

| Query | Index slice | Filtered afterwards |
|---|---|---|
| `city = SP` | all entries with city `SP` | nothing |
| `city = SP age >= 18 age < 30` | city `SP`, age in [18, 30) | nothing |
| `age > 30` | age is not the first field, so the whole index | `age > 30` |
| `city = SP name = ana` | city `SP` | `name = ana` (read from each hash) |
| (no condition) | the whole index | nothing |

Put the field you always filter by equality first, and the field you query by range right after it.

## IDX.CREATE

```text
IDX.CREATE index [ON HASH | ON JSON] [PREFIX count prefix ...]
    SCHEMA field-spec [field-spec ...] [UNIQUE]

field-spec (hash):  field TAG [SEPARATOR c] | field NUMERIC
field-spec (JSON):  path [AS name] TAG [SEPARATOR c] | path [AS name] NUMERIC
```

- `ON HASH` is the default. `ON` and `PREFIX`, when present, come in this order, before `SCHEMA`.
- `PREFIX count prefix ...` lists the key prefixes the index covers.
- On a hash, the field name is the hash field. On JSON, the field is a JSONPath or legacy path (see
  [json.md](json.md#paths)); `AS name` gives it the name queries use, and without `AS` the name is the path
  text itself.
- `SEPARATOR c` (TAG only, one character) splits a value into several (see [Field values](#field-values)).
- `UNIQUE`, the last argument, makes the index reject two keys with the same tuple (see [UNIQUE](#unique)).
- The index name space is shared with `FT.CREATE` indexes.
- Reply: `OK`. The index is registered at once, and existing keys are indexed in the background (see
  [Backfill and index states](#backfill-and-index-states)).

Errors:

| Error | Cause |
|---|---|
| `ERR Index already exists` | an `IDX` or `FT` index has this name |
| `ERR ON takes HASH or JSON` | |
| `ERR bad PREFIX count` | the count is not a number or exceeds the arguments |
| `ERR expected SCHEMA` | |
| `ERR unknown field type 'X' (use TAG or NUMERIC)` | |
| `ERR SEPARATOR takes a single character` | |
| `ERR duplicate field 'x'` | two fields with the same name |
| `ERR SCHEMA needs at least one field` | |
| `ERR an index has at most 8 fields` | |
| `ERR invalid path '...': ...` | a JSON field path does not parse |
| `ERR syntax error` | a field without a type, or arguments after `UNIQUE` |

## Field values

The values a key has for a field decide its entries.

**Hash fields**

- `TAG` without separator: the field's value exactly as stored (case-sensitive, not trimmed).
- `TAG` with `SEPARATOR c`: the value is split at `c`, each piece trimmed of spaces and tabs, empty pieces
  dropped, duplicates removed. `"c++, go"` with `SEPARATOR ,` gives `c++` and `go`.
- `NUMERIC`: the value, trimmed, parsed as a number. A value that is not a number leaves the key out of the
  index.

**JSON fields.** The values are what the field's path selects in the document:

- A string counts as it is; a number or boolean counts as its JSON text (`30`, `true`).
- An array counts as each of its elements (`$.tags` on `["db","cpp"]` gives `db` and `cpp`), and so does a
  path that matches several times (`$.items[*].sku`).
- `null`, objects, and nested arrays inside a selected array are ignored.
- `SEPARATOR` then splits string values as on a hash.
- If no value is left, the document is not in the index, like a hash without the field.

**Several values.** A field with several values gives the key one entry per combination of the values of all
fields. A key with `skills = [go, c++]` and `level = 3` has two entries, `(go, 3)` and `(c++, 3)`. At most 1024
combinations per key and index are kept; a key with more is left out of the index entirely.

- `skills = go` finds the keys that have `go` among their skills.
- Two conditions on the same field require both values: `skills = go skills = c++`.
- A key appears once in a query result and counts once in `IDX.COUNT`, however many of its entries match.

## Querying: IDX.QUERY and IDX.COUNT

```text
IDX.QUERY index [field op value ...] [DESC] [LIMIT offset count] [NOCONTENT | RETURN count name ...]
IDX.COUNT index [field op value ...]
```

**Conditions** are triples `field op value`, with `op` one of `=` (or `==`), `>`, `>=`, `<`, `<=`. Conditions
and options may be interleaved; options are case-insensitive.

- `field` is an index field name or, for filtering only, any other name. On a hash it is a hash field. On a
  JSON index it is an index field name, a path (`$.name`, `.name`), or a bare name, which means the top-level
  member (`name` is `$.name`).
- On an index field the comparison uses the field's type: numeric for `NUMERIC`, byte order for `TAG` (after
  splitting multi-valued values; the condition holds if any value satisfies it).
- On any other field, `=` compares the text exactly and the range operators compare numerically (a value that
  is not a number fails the condition). JSON values that are not strings are compared as their JSON text.
- A key without the field fails the condition.
- A non-numeric value on a `NUMERIC` field used for the index slice fails the query with
  `ERR 'abc' is not a number (field 'age' is NUMERIC)`.

**Options**

| Option | Effect |
|---|---|
| `DESC` | Reverse index order. |
| `LIMIT offset count` | Skip `offset` matches, return up to `count`. Default `LIMIT 0 10`. A negative `count` means no limit; `count` 0 returns an empty page (use `IDX.COUNT` to count). |
| `NOCONTENT` | Return keys only. |
| `RETURN count name ...` | Return only these fields. Names follow the same rules as condition fields. |

`NOCONTENT` and `RETURN` together fail with `ERR NOCONTENT and RETURN cannot be used together`.

**Replies**

| Form | Reply |
|---|---|
| default, hash | `[key1, [field, value, ...], key2, [...], ...]` with every field of each hash |
| default, JSON | `[key1, ["$", document], ...]`, the whole document as compact JSON |
| `RETURN` | `[key1, [name, value, ...], ...]` with only the names that exist in that key; on JSON, the first value the path selects (a string as is, anything else as JSON text) |
| `NOCONTENT` | `[key1, key2, ...]` |
| `IDX.COUNT` | an integer |

```text
127.0.0.1:6379> IDX.QUERY users city = RJ
1) "user:3"
2) 1) "age"
   2) "41"
   3) "city"
   4) "RJ"
   5) "email"
   6) "caio@x"
   7) "name"
   8) "caio"
127.0.0.1:6379> IDX.QUERY users city = RJ RETURN 1 name
1) "user:3"
2) 1) "name"
   2) "caio"
127.0.0.1:6379> IDX.COUNT users
(integer) 3
```

**Errors**

| Error | Cause |
|---|---|
| `ERR no such index 'x'` | |
| `ERR 'x' is a search index; query it with FT.SEARCH` | the name belongs to an `FT.CREATE` index |
| `ERR index 'x' is still being built (N keys scanned)` | backfill in progress |
| `ERR index 'x' failed: <reason>` | the index is in the failed state |
| `ERR syntax error near 'X' (conditions are: field op value, op one of = > >= < <=)` | an unknown option or a malformed condition |
| `ERR bad RETURN count` | |
| `ERR value is not an integer or out of range` | a bad `LIMIT` |

`IDX.COUNT` accepts conditions only, no options.

**What a query reads.** Each entry carries the key's collection version and TTL, so `IDX.COUNT` and
`IDX.QUERY ... NOCONTENT` without extra filters read only the index. When a query needs field values (the
default content, `RETURN`, or filters), it reads each key from a RocksDB snapshot and checks the entry against
it. An entry that no longer matches its key is skipped and repaired (up to 64 repairs per query). Expired keys
are skipped. Before reading, the query commits the connection's pending pipelined writes, so a client always
sees its own writes.

## IDX.GET

```text
IDX.GET index value [value ...]
```

An exact lookup with one value per index field, in field order (`ERR index 'x' has N fields; give one value
for each` otherwise). Values are matched like equality conditions.

- On a `UNIQUE` index: `[key, [field, value, ...]]` (or `["$", document]` on JSON), or nil if no key has
  these values.
- On another index: every match, in the `IDX.QUERY` default shape, possibly an empty array.

## IDX.INFO, IDX.LIST and IDX.DROP

```text
IDX.INFO index
IDX.LIST
IDX.DROP index
```

`IDX.INFO` replies with a map (a flat array in RESP2):

| Field | Value |
|---|---|
| `name` | the index name |
| `kind` | `compound` for `IDX.CREATE`, `search` for `FT.CREATE` |
| `on` | `HASH` or `JSON` |
| `state` | `building`, `ready` or `failed` |
| `prefixes` | array of prefixes (empty: every key) |
| `fields` | array of `[name, type]`, plus the path on JSON, plus `"SEPARATOR", c` when set |
| `unique` | 1 or 0 |
| `keys_scanned` | keys visited by the backfill |
| `error` | only in the `failed` state: why it failed |

`IDX.LIST` returns the names of all indexes, `IDX.*` and `FT.*` alike, sorted by name.

`IDX.DROP` removes the definition and all its entries at once and replies `OK`, or `ERR no such index 'x'`. It
also drops `FT.CREATE` indexes. The data is not touched.

## Consistency

- **Same batch.** Index entries are written in the same RocksDB write batch as the data change, so the index and
  the data never diverge, even after a crash.
- **Hash writes** that maintain entries: `HSET`, `HMSET`, `HSETNX`, `HDEL`, `HINCRBY`, `HINCRBYFLOAT`, `DEL`,
  `UNLINK`, the `EXPIRE` family, `PERSIST`, `RENAME`, and blind `SET`/`MSET` (or memcache writes) over a hash,
  which remove its entries.
- **JSON writes** that maintain entries: every `JSON.*` write; `SET`, `MSET` or memcache writes over a
  document; a collection command that replaces a document (for example `SUNIONSTORE` into its key); `DEL`,
  `EXPIRE`/`PERSIST` and `RENAME`.
- **TTL.** Entries carry the key's expiry time, so an expired key disappears from results immediately, before
  any deletion.
- **Cleanup.** A RocksDB compaction filter removes entries whose key no longer exists, has expired, has a new
  collection version, or has changed type, and entries of dropped indexes.
- **FLUSHALL** removes all entries; index definitions remain.
- **Replicas** receive entries and definitions from the primary (see [replication.md](replication.md)).

Internals: [dev/indexes-internals.md](dev/indexes-internals.md).

## UNIQUE

- No two keys may have the same tuple in a `UNIQUE` index (for a multi-valued field, the same value). A key may
  keep or rewrite its own value.
- A write that would break this fails as a whole, nothing of the command is applied, and it replies
  `ERR UNIQUE index 'emails' already has this value, for key 'user:1'`.
- Writers to a `UNIQUE` index take the index's lock and commit their entry before releasing it, so two
  connections cannot claim the same value at once. This also means writes to a `UNIQUE` index are committed
  right away instead of waiting for the rest of the pipeline, which costs throughput.
- An entry whose owner was overwritten, deleted or expired does not block another key.
- `RENAME` moves the key's value to the new name; it fails if the new name would conflict.

## Backfill and index states

- `IDX.CREATE` registers the index; from then on every write maintains it. A background task then visits every
  existing key under the prefixes, in chunks of 256 keys per write batch.
- While the backfill runs, the state is `building` and `IDX.QUERY`, `IDX.COUNT` and `IDX.GET` are refused. Poll
  `IDX.INFO` until `state` is `ready`.
- If the existing data has duplicate values for a `UNIQUE` index, the backfill stops and the index becomes
  `failed`, with the reason in `IDX.INFO`. A failed index is no longer maintained and refuses queries; fix the
  data, then `IDX.DROP` and create it again.
- Definitions are stored in RocksDB and survive restarts. A backfill interrupted by a restart starts over
  automatically.

```text
127.0.0.1:6379> IDX.CREATE users PREFIX 1 user: SCHEMA city TAG age NUMERIC
OK
127.0.0.1:6379> IDX.QUERY users city = SP
(error) ERR index 'users' is still being built (512 keys scanned)
127.0.0.1:6379> IDX.INFO users
...
 7) "state"
 8) "ready"
...
```

## Permissions

- `IDX.CREATE` and `IDX.DROP` are in `@write`, `@admin`, `@dangerous`, `@slow` and `@index` (`@search` is an
  alias of `@index`). `IDX.QUERY`, `IDX.COUNT`, `IDX.GET`, `IDX.INFO` and `IDX.LIST` are in `@read`, `@slow` and
  `@index`.
- `IDX.QUERY`, `IDX.COUNT` and `IDX.GET` return and count only the keys the user may read. A user limited to
  `~app:*` does not learn about keys outside it. Redis modules return every key; datadam filters.
- On a replica, `IDX.CREATE` and `IDX.DROP` are refused with `READONLY`.

See [security.md](security.md) for ACL rules.

## Costs

Measured in Docker with 100 000 hashes and 50 clients:

| Operation | thousand ops/s |
|---|---|
| `HSET` with `--pipe`: no index / 1 index / 1 index + 1 `UNIQUE` | 224 / 170 / 126 |
| `IDX.GET` on a `UNIQUE` index, no pipeline / pipeline 16 | 139 / 641 |
| `IDX.QUERY city = c42 age < 30 LIMIT 0 10`, no pipeline / pipeline 16 | 92 / 158 |
| `IDX.QUERY city = c42 LIMIT 0 100 NOCONTENT` | 95 |
| `IDX.COUNT` over 1 000 hashes | 41 |

On JSON documents of about 40 bytes (100 000 keys, 100 connections): `JSON.SET` runs at 315 thousand/s without
an index and 289 thousand/s with a two-field index; `JSON.NUMINCRBY` on an indexed field at 213 thousand/s;
`IDX.QUERY city = SP LIMIT 0 10` returning the documents at 106 thousand/s.

## IDX or FT?

Both index types can cover the same prefix at the same time.

| | `IDX.*` | `FT.*` ([search.md](search.md)) |
|---|---|---|
| Shape | one compound index over all its fields | each field indexed separately |
| Keys missing a field | left out | still indexed (found by `*` and by the other fields) |
| Query | equality on leading fields + range on the next; other conditions filter | any combination of fields with AND, OR, NOT |
| Total count | not computed (`IDX.COUNT` is separate) | always computed, so every matching entry is read |
| `UNIQUE` | yes | no |
| During backfill | queries refused | queries answer with what is indexed so far |
| Client compatibility | datadam-specific | RediSearch syntax |

Use `IDX.*` for a fixed, frequent access pattern (a lookup by email, "orders of customer X by date"): it reads
only the slice of the page and was about 20 times faster than `FT.SEARCH` for the same combination of fields
(see [search.md](search.md#costs)). Use `FT.*` for ad hoc combinations of fields, for queries that need OR or
NOT, or for clients that already speak RediSearch.
