# Secondary index internals

This page explains how the secondary indexes behind `IDX.*` (compound indexes) and `FT.*` (RediSearch-compatible
search indexes) are defined, stored, kept in step with the data, queried and garbage collected. The user-facing
behavior is in [secondary-indexes.md](../secondary-indexes.md) and [search.md](../search.md); this page is about
the code in [`src/index/`](../../src/index/), [`src/db/index_ops.cpp`](../../src/db/index_ops.cpp),
[`src/db/search_ops.cpp`](../../src/db/search_ops.cpp) and the `KeyTxn` parts of
[`src/db/database.cpp`](../../src/db/database.cpp).

## Contents

- [Overview](#overview)
- [IndexDef](#indexdef)
- [Definition encoding](#definition-encoding)
- [Entry layout](#entry-layout)
- [Value encodings](#value-encodings)
- [Tuple sets](#tuple-sets)
- [IndexRegistry](#indexregistry)
- [Maintenance](#maintenance)
- [Backfill](#backfill)
- [IDX queries](#idx-queries)
- [FT queries](#ft-queries)
- [Garbage collection](#garbage-collection)
- [Known limitations and ideas](#known-limitations-and-ideas)
- [Tests](#tests)

## Overview

Both kinds of index share one design:

- An index covers the keys that start with one of its prefixes (all keys if it has none) and reads its fields
  from hashes (`IndexSource::Hash`) or from JSON documents (`IndexSource::Json`).
- Each covered key has a set of *tuples* in the index. Every tuple is one entry in the `index` column family,
  written in the same RocksDB write batch as the change to the key, so a crash can never leave the index and
  the data out of step.
- An entry value carries the owner's collection version and TTL, so an entry can be trusted, or recognized as
  stale, without reading the owner.
- Definitions live in the `System` column family and are loaded by `IndexRegistry`.

The kinds differ in what a tuple is:

| | Compound (`IDX.CREATE`) | Search (`FT.CREATE`) |
|---|---|---|
| `IndexKind` | `Compound` (0) | `Search` (1) |
| Tuple | encoded value of every field, in field order | `u8` field position + encoded value, one per value; plus `kPresence` |
| Key missing a field | not indexed | indexed with what it has |
| Fields | up to `kMaxFields` = 8 | up to `kMaxSearchFields` = 128 |
| UNIQUE | supported | not offered by `FT.CREATE` |
| Query | one contiguous slice plus filters | set operations over scans of several fields |

## IndexDef

`struct IndexDef` in [`index_def.h`](../../src/index/index_def.h):

| Member | Meaning |
|---|---|
| `id` | `u32`, assigned by `IndexRegistry::create`, never reused |
| `name` | index name |
| `source` | `IndexSource::Hash` (0) or `IndexSource::Json` (1); `json()` |
| `kind` | `IndexKind::Compound` (0) or `IndexKind::Search` (1); `search()` |
| `prefixes` | key prefixes; empty means every key. `matches(key)` |
| `fields` | `std::vector<Field>` |
| `unique` | compound only: one key per tuple |
| `state` | `IndexState::Building` (0), `Ready` (1), `Failed` (2) |
| `error` | why the index failed |
| `paths_` | private: the compiled `JsonPath` of each field (`compile()`) |

`IndexDef::Field`:

| Member | Meaning |
|---|---|
| `name` | the name queries use: a hash field, an alias (`AS`), or for JSON the path itself when there is no alias |
| `type` | `IndexFieldType::Tag` (0) or `IndexFieldType::Numeric` (1) |
| `path` | JSON: the JSONPath or legacy path. Hash: the real field name when `name` is an alias (empty otherwise) |
| `separator` | TAG: splits a value into several (0: none) |
| `case_sensitive` | TAG: `false` folds ASCII letters to lower case (`normalize()`) |

Defaults differ between the commands. `IDX.CREATE` ([`cmd_index.cpp`](../../src/server/cmd_index.cpp)) leaves
`separator = 0` unless `SEPARATOR c` is given and keeps `case_sensitive = true`; it has no `AS` for hashes.
`FT.CREATE` ([`cmd_search.cpp`](../../src/server/cmd_search.cpp)) makes TAG fields case insensitive unless
`CASESENSITIVE`, with separator `,` on hashes and none on JSON, and sets `path` for every JSON field and for
aliased hash fields. `hash_field(i)` returns `path` if set, else `name`.

`multi_valued()` is true for every JSON index and for any index with a separator: a key may then have several
tuples and query scans must skip repeated owners. `field_index(name)` finds a field by its query name.

## Definition encoding

`IndexDef::encode()` writes the current format (`kDefFormat = 3`); `IndexDef::decode()` still reads formats 1
and 2. All integers are big endian; a string is `u32 length | bytes`.

```text
u8   format                     1, 2 or 3
u8   source                     format >= 2 (IndexSource; format 1 means Hash)
u8   kind                       format >= 3 (IndexKind; formats 1-2 mean Compound)
u32  id
str  name
u32  prefix count,  then  str prefix  x count
u32  field count (decode rejects more than kMaxSearchFields),  then per field:
       str  name
       u8   type                (IndexFieldType, 0 or 1)
       str  path                format >= 2
       u8   separator           format >= 3 (0: none)
       u8   flags               format >= 3: bit kFieldCaseFolded (1) = not case sensitive
u8   unique                     0 or 1
u8   state                      IndexState, 0..2
str  error
```

`decode()` fails on trailing bytes and on JSON paths that do not compile. `IndexRegistry::load` logs and skips a
definition it cannot decode. The definition is stored under `index:def:<name>` in the `System` column family,
next to `index:next-id` (see [storage-format.md](storage-format.md#system-column-family)).

## Entry layout

```text
normal:  u32 BE index id | tuple | owner key   ->  be64 version | be64 expire_at_ms
UNIQUE:  u32 BE index id | tuple               ->  be64 version | be64 expire_at_ms | owner key
```

- `entry_key(tuple, owner)` and `entry_value(version, expire_at_ms, owner)` build them; `id_prefix()` is
  `encode_index_id(id)`.
- `version` is the owner hash's `CollectionMeta::version`. JSON documents have no collection version and use 0;
  every write of a document rewrites its entries instead.
- `expire_at_ms` is the owner's TTL (0: none), so expired owners are skipped without reading them.
- In a UNIQUE index the owner goes into the value, so "is this value taken?" is one point read of
  `id | tuple`.
- Search entries start with the field byte: `field_prefix(f)` is `id | u8 f` and `presence_prefix()` is
  `id | 0xFF`.

`split_entry(key, value, &Entry)` parses an entry. The tuple length is found by walking the encodings: for a
compound index, one encoded value per field; for a search index, the field byte and then one value of that
field's type (or nothing after `kPresence`, with `Entry::field = -1`). It fails if the value is shorter than 16
bytes, a field byte is out of range, or an encoding is malformed.

## Value encodings

`encode_index_value(type, value, &out)` appends a self-delimiting encoding whose byte order matches the value
order, so that compound tuples compare field by field:

| Type | Encoding |
|---|---|
| NUMERIC | `be64(sortable_score(parse_double(value)))`, always 8 bytes; fails if the value is not a number |
| TAG | the bytes, with each `0x00` written as `0x00 0xFF`, then the terminator `0x00 0x01` |

The TAG terminator sorts before every continuation of the value (an escaped zero `0x00 0xFF`, or any byte other
than zero), so a value sorts before every longer value that starts with it, and the encodings of different
values never interleave. Helpers:

- `tag_prefix(v)`: the encoding of `v` without its terminator. Every value that starts with `v` encodes to
  something that starts with it (prefix queries, `@f:{pre*}`).
- `tag_bound(v, after)`: the encoding with its terminator; with `after = true` the terminator becomes
  `0x00 0x02`, which sorts after every entry whose value is exactly `v`.
- `decode_tag(s, &out)`: decodes the TAG value at the start of `s`.
- `prefix_end(p)`: the smallest string greater than every string that starts with `p` (strip trailing `0xFF`
  bytes, increment the last); `""` means "no bound". Also used for key prefixes in `SCAN` and the backfill.

## Tuple sets

`IndexDef::Tuples` is a `std::vector<std::string>` kept sorted and without duplicates (empty: the key is not
indexed); `contains_tuple()` is a binary search. Tuples come from the raw field values:

- `split_values(field, raw, &out)`: NUMERIC takes the value trimmed of spaces and tabs. TAG without a separator
  takes the whole value, normalized. TAG with a separator splits on it, trims each piece, drops empty pieces and
  normalizes the rest. Duplicates are dropped.
- `json_values(field, doc)`: every match of the field's compiled path; an array match contributes each of its
  elements. A string is passed to `split_values` as it is, a number or boolean as its JSON text (`dump_json`);
  null, objects and nested arrays are skipped.
- `tuples(value_of)` reads each `hash_field(i)` through `value_of`. A compound index returns no tuples as soon
  as one field is missing; a search index takes what there is. `json_tuples(doc)` does the same for documents.
- `make_tuples(values)` builds the set:
  - compound: the cartesian product of the encoded values of each field, in field order. A value that does not
    encode (a NUMERIC that is not a number) is dropped; if a field is left with no value, or the product would
    exceed `kMaxTuples` (1024), the key is not indexed at all.
  - search: one tuple `kPresence` (`0xFF`), then `u8 field position | encoded value` for every value of every
    field (values that do not encode are skipped). If there are more than `kMaxTuples`, the list is cut to the
    first 1024 in that order (presence first), so a search index keeps a key with too many values but loses
    some of them.

Example: a compound index on `(skills TAG SEPARATOR ",", level NUMERIC)` and the hash
`skills = "c++, go"`, `level = 3` has two tuples, `enc("c++") enc(3)` and `enc("go") enc(3)`, hence two entries.

## IndexRegistry

[`index_registry.h`](../../src/index/index_registry.h) holds the live indexes as
`std::shared_ptr<IndexRegistry::Index>`:

```cpp
struct Index {
  IndexDef def;                                 // fixed after creation, except state and error
  std::atomic<IndexState> state;
  std::atomic<uint64_t> scanned;                // keys looked at by the backfill
  std::atomic<bool> dropped;
  std::mutex unique_mu;                         // writers of a UNIQUE index check and commit under it
};
```

- `by_name_` and `by_id_` are guarded by a `shared_mutex`; `count_` makes `any()` lock-free, so writes without
  indexes pay nothing.
- `create(def, &out)` rejects a duplicate name, assigns `next_id_++`, persists `index:next-id` and then the
  definition (two separate `RocksStorage::put` calls) in state `Building`.
- `drop(name)` marks the index `dropped`, removes it from the maps and writes one batch that deletes
  `index:def:<name>` and `DeleteRange`s `[id_prefix, prefix_end(id_prefix))` in the `index` column family.
- `matching(key)` returns the indexes in state `Building` or `Ready` whose prefixes match, sorted by id (the
  order UNIQUE locks are taken in); `covers(key)` is the boolean form. Failed indexes are not maintained.
- `set_state(index, state, error)` updates and persists a definition (ignored once dropped).
- `reload()` re-reads all definitions. Existing ids keep their `Index` object (state and error are updated),
  new ids get one, and ids that disappeared are marked `dropped`. Replicas call it after applying a batch that
  touched a `System` key starting with `index:`, and `Database::promote` and `clear_for_full_sync` call it too.
- `owner_of(key, value, ...)` implements `IndexOwnerResolver` for the compaction filter (see
  [Garbage collection](#garbage-collection)).

Ids are never reused: `index:next-id` only grows and `load()` also takes `max(def.id + 1)`. An entry left
behind by a dropped index can therefore never be read as an entry of a new one.

## Maintenance

Index entries are written by whichever code path writes the key, inside the same `RocksStorage::Batch` or
`PendingBatch`. There are three families of paths.

### Collections: KeyTxn

Collection commands run in `Database::transact` / `transact_many` (see [concurrency.md](concurrency.md)). A
`KeyTxn` collects `indexes_ = matching(key)` when it is created, then:

1. `capture()` runs before the first change (`put`, `del`, `reset`, `remove`). It records the current tuples of
   every matching index (`current_tuples`), the collection version (`old_version_`) and the TTL (`old_expire_`).
   `current_tuples` reads hash fields through `KeyTxn::get`, so it sees the elements in memory or the pending
   batch, and parses the record for JSON indexes.
2. The command runs and changes elements and meta in the transaction.
3. `TxnRun::finish()` collects the UNIQUE indexes the transactions may write to (`unique_indexes()`), sorts them
   by id and locks each `unique_mu`.
4. `prepare_indexes()` computes `changes_` without writing anything. For each index it compares the old tuples
   with `current_tuples` now:
   - if the version and TTL are unchanged (`same_value`) and the tuple sets are equal, nothing happens;
   - every old tuple not in the new set is deleted (`own_delete`: in a UNIQUE index only if the entry still names
     this key);
   - every new tuple is written with `entry_value(meta_.version, expire, key)`. If the version or TTL changed,
     all current tuples are rewritten, because the value carries both;
   - for a UNIQUE index, a new tuple whose entry exists and whose `live_unique_owner()` is another key throws
     `IndexConstraintError("ERR UNIQUE index '<name>' already has this value, for key '<owner>'")`.
5. `apply_indexes()` stages the changes with `stage_index` (recorded in the batch's undo log).
6. `KeyTxn::finish()` writes the meta record.
7. If any UNIQUE lock is held, the batch is committed before the locks are released, so the next writer of that
   index sees these entries in RocksDB.

If `prepare_indexes` throws, `~TxnRun` replays the undo log backwards (data and index stages of this
transaction), truncates `range_deletes` and marks the transaction's cached elements as stale. Because the meta
record is only written in step 6, the key is unchanged and the command fails with the constraint error.

`KeyTxn::reindex(index)` (backfill) and `KeyTxn::repair(index, entry)` (query repair) plan with an empty old set
and `force = true`, so all current tuples are written; `repair` also deletes the given entry unless it is one of
the key's current entries.

`live_unique_owner(def, value)` decides whether an existing UNIQUE entry is taken: for a hash index, the owner
must be a live hash (`stored_hash_meta`, which also sees this thread's pending writes) with the version stored
in the entry; for a JSON index, the owner must hold a live `Json` record. A stale entry is simply overwritten.

### Hashes written outside KeyTxn

Commands that replace or remove a record without a `KeyTxn` go through `Database::update`, `put`, `mset` or
`rename`. For hashes they use two helpers in [`index_ops.cpp`](../../src/db/index_ops.cpp), which read the
hash's tuples with `stored_tuples` (RocksDB plus this thread's pending data writes):

- `drop_index_entries(key, meta, batch)` deletes every entry of the hash (UNIQUE entries only if they name the
  key). Used by `update` when a hash is deleted or replaced by another version, by `put` (a blind `SET` over a
  hash), by `mset` and by `rename` (for both source and old destination).
- `refresh_index_entries(key, meta, expire_at_ms)` rewrites the entries with the new TTL. Used by `update` when
  only the TTL of a hash changed (`EXPIRE`, `PERSIST`, ...).

`rename` moves a hash's entries: the source's are deleted, and for every index matching the destination the
source's tuples are written under the new owner with the version the moved hash gets (`next_version()`),
checking UNIQUE indexes of the destination under their locks.

### JSON documents

A document has no elements and no version; its entries are recomputed from the document on every write.
`Database::json_entries(key, rec)` returns the JSON indexes matching the key and, if the record is a live
document, the tuples of each and its TTL. `json_entry_changes(key, before, after, also_mine)` diffs two such
snapshots: tuples that disappeared are deleted (UNIQUE: only if the entry names the key), tuples that appeared
are written with version 0, and if the TTL changed every tuple is rewritten. A UNIQUE value held by a live key
other than `key` and `also_mine` throws `IndexConstraintError`.

| Path | What it does |
|---|---|
| `Database::update` (all `JSON.*` writes; string writes that read the old record, such as `SET` with options, `INCR`, `APPEND`; `EXPIRE`, `PERSIST`, `DEL`; memcache) | `json_entries` before `f(rec)`; after it, unless `Op::Keep`, `json_entries` again (or `gone()` for a delete), `lock_unique`, `write_index` for each change, then the record; commits the batch if UNIQUE locks are held |
| `Database::put` (blind writes such as a plain `SET`, under an index prefix) | same, from the old record to the new one |
| `Database::mset` | deletes the entries of old documents in its own batch |
| `Database::rename` | deletes the source's entries and writes the destination's from the moved document (`also_mine = src`) |
| `KeyTxn` (a collection command replacing a document) | `capture()` sees the document's tuples, the new hash has none, so they are deleted |

`Database::update` opens a local `WriteBatchScope` whenever any index exists, so the entries and the record
always reach RocksDB together.

## Backfill

`Database::create_index(def, scan)` registers the index (from that moment every write maintains it) and, with
`scan`, starts a thread running `run_backfill`; without it (`FT.CREATE ... SKIPINITIALSCAN`) the index is set
`Ready` at once.

`run_backfill(index)`:

1. For each prefix of the index (or `""`), it reads up to `kBackfillChunk` = 256 keys from the `meta` column
   family (no snapshot, `fill_cache = false`) starting at `from`.
2. In one `WriteBatchScope`, for each key it runs `transact(key, [&](KeyTxn& t) { t.reindex(index); })` and
   increments `index->scanned` (shown by `IDX.INFO` / `FT.INFO`), then commits.
3. `from` becomes the last key plus `'\0'`; it stops when a chunk is empty.
4. It sets the state `Ready`. An `IndexConstraintError` (duplicate values in a UNIQUE index) or any other
   exception sets `Failed` with the message.

It returns early if the database is stopping or the index is dropped. Because writes maintain the index while
the scan runs, the index is complete once the scan ends; re-indexing a key twice is harmless. The `Database`
constructor restarts the backfill of every index still `Building` (from the beginning: progress is not
persisted); replicas pass `resume_backfills = false` and receive the entries from their primary, and
`Database::promote` starts the interrupted backfills. Threads are kept in `backfills_` and joined by
`~Database`.

## IDX queries

`Database::query_index(const IndexQuery&)` serves `IDX.QUERY`, `IDX.COUNT` and `IDX.GET` (an `IDX.GET` is an
equality query on every field, with `limit` 1 for a UNIQUE index). It refuses search indexes and indexes that
are `Building` or `Failed`.

### Planning

`plan_query(def, where, &plan)` turns the conditions into one key range and a list of filters:

1. Walk the fields in order while there is an unused `=` condition on the field: append
   `encode_index_value(type, normalize(value))` to the prefix (an invalid NUMERIC is an error).
2. `lower = prefix`, `upper = prefix_end(prefix)`.
3. For the next field `k`, every range condition on it narrows the range. NUMERIC bounds are
   `prefix | be64(sortable_score(v))`, plus 1 for `>` and `<=` (the bound "just past" `v`). TAG bounds are
   `prefix | tag_bound(normalize(v), after)` with `after` for `>` and `<=`. `>`/`>=` raise `lower`, `<`/`<=`
   lower `upper`.
4. Every condition not used becomes a filter; `plan.empty` is set when `lower >= upper`.

So `city = SP age >= 18 age < 30` on `(city, age)` reads exactly the entries between
`id | enc(SP) | be64(sortable(18))` and `id | enc(SP) | be64(sortable(30))`.

### Execution

The query commits the caller's pending batch (its own writes must be visible), takes a RocksDB snapshot and
iterates the range (backwards with `DESC`). For each entry:

1. Skip it if it does not split or `Entry::expired(now)`, or if `key_filter` (ACL key patterns) rejects the
   owner, or if the owner was already returned (`seen`, only for `multi_valued()` indexes).
2. If no owner data is needed (`IDX.COUNT`, `NOCONTENT` without filters or `RETURN`), the entry is trusted as
   it is: the index alone answers.
3. Otherwise the owner is read from the snapshot and checked:
   - hash: `stored_hash_meta` must give a live, non-empty hash with the entry's version; then the needed fields
     (or all of them, for full content) are read and the entry's tuple must be among `def.tuples(...)`;
   - JSON: `stored_json` must give a document whose `json_tuples` contain the entry's tuple.
   A failed check records `(owner, entry)` as stale and skips the entry.
4. Filters are evaluated with `condition_holds`: on an index field against all its values (split and
   normalized), elsewhere against the field (hash) or the path's values (JSON; a bare name means `$.name`). A
   NUMERIC field, or a range operator on a non-index field, compares as numbers; anything else compares bytes.
5. `take()` applies `OFFSET` and `LIMIT` (`limit < 0`: no limit) or counts.

After the scan, up to `kMaxRepairsPerQuery` = 64 stale entries are repaired, each with
`transact(owner, [&](KeyTxn& t) { t.repair(index, entry); })`: the stale entry is deleted unless it is current,
and the owner's current entries are rewritten. An `IndexConstraintError` during repair is ignored (the owner's
next write will deal with it). On a replica the repair's writes are dropped by `commit_batch`, like every write
made by a read.

## FT queries

### Parsing

`parse_search_query(text, def, params, &error)` in [`search_query.cpp`](../../src/index/search_query.cpp) is a
recursive descent parser (`QueryParser`) producing a `SearchNode` tree:

```cpp
struct SearchNode {
  enum class Kind { All, Tag, Range, And, Or, Not };
  size_t field;                                   // Tag, Range
  std::vector<std::string> values;                // Tag: normalized values
  std::vector<bool> prefix;                       // Tag: value i ends with '*'
  double lo = -inf, hi = +inf; bool lo_open, hi_open;   // Range
  std::vector<SearchNode> children;               // And, Or, Not (one child)
};
```

Grammar, loosest binding first:

```text
and    := or+                        juxtaposition: intersection
or     := unary ('|' unary)*         union, binds tighter than juxtaposition
unary  := '-' unary | '~' unary | atom        '-': Not; '~' (optional clause) is ignored
atom   := '(' and ')' | '*' | '@' field ':' ( '{' tags '}' | '[' bound [','] bound ']' )
```

- Field names may contain `\`-escaped characters and must name a field of the index; a TAG field takes `{...}`
  and a NUMERIC field `[...]`. A bare word is rejected with a message pointing at the lack of full-text search.
- Tags are separated by `|`, may use `\` escapes or `"..."` quoting, and a value ending with an unescaped `*` is
  a prefix. `$name` at the start of a tag is replaced by a `PARAMS` value. Values are normalized like the field.
- Bounds accept `(` for exclusive, `-inf`, `+inf`/`inf` and `$name`; NaN is rejected.
- Errors read `ERR Syntax error at offset N: ...`.

`cmd_ft_search` refuses a `Failed` index but answers while one is `Building` (with what is indexed so far, as
RediSearch does), and wraps the query in an `And` with each legacy `FILTER field min max` clause (parsed as
`@field:[min max]`).

### Evaluation: SearchScanner

`Database::search(index, query, opts)` in [`search_ops.cpp`](../../src/db/search_ops.cpp) commits the caller's
pending batch, takes a snapshot and creates a `SearchScanner` over it. Results are `Keys`: sorted vectors of
owner keys without duplicates.

- `scan(lower, upper, reverse, fn)` visits the entries in a range that split and are not expired. Entries are
  not checked against their owners (see [limitations](#known-limitations-and-ideas)).
- `owners(lower, upper, &out, within)` collects the owners of a range; with `within`, only owners found in it
  by binary search. The whole range is still scanned.
- `all()` collects the owners of the presence entries, `id | 0xFF | owner`, which are already in key order.
- `eval(node, within)`:
  - `All`: `*within`, or `all()`.
  - `Tag`: for each value, the range `field_prefix | enc(v)` (exact, terminator included) or
    `field_prefix | tag_prefix(v)` (prefix), up to `prefix_end`; then sorted and deduplicated.
  - `Range`: from `field_prefix | be64(sortable(lo) + lo_open)` (or the field prefix itself for an inclusive
    `-inf`) to `field_prefix | be64(sortable(hi) + !hi_open)` (or the end of the field for an inclusive `+inf`).
  - `Or`: the union of the children, each evaluated with the same `within`.
  - `Not`: `base - eval(child, &base)` where `base` is `within` or `all()`.
  - `And`: the positive children, stable-sorted with TAG clauses first (usually the most selective); the first
    is evaluated with `within`, each next one with the result so far as `within`, stopping as soon as it is
    empty. With no positive child the start is `within` or `all()`. Then each `Not` child's result (evaluated
    within the current result) is subtracted.

### Paging, sorting and reading documents

- Fast path: a bare `*` without `SORTBY` scans the presence entries once, counting every owner that passes
  `key_filter` and keeping only the page.
- Otherwise `eval(query)` gives every match; `key_filter` is applied and `total` is the number of keys left.
  With `SORTBY` (and a non-empty page), the sort field's entries are scanned in order (reversed for `DESC`) and
  matching owners are taken the first time they appear until `offset + limit` keys are ordered; keys without the
  field follow in key order. Without `SORTBY` the order is key order.
- Only the page is read, from the same snapshot: the whole document (`"$"`) or the `RETURN` paths for JSON, and
  all fields or the `RETURN` fields for hashes. A key that disappeared since the scan is skipped (it still
  counts in `total`).

### FT.INFO, FT.TAGVALS, FT.DROPINDEX DD

- `count_search_entries` scans every live entry of the index: `records` counts them, `num_docs` counts the
  presence entries.
- `tag_values(index, field)` scans the field's entries and decodes each TAG value, dropping consecutive
  duplicates (values are stored normalized).
- `FT.DROPINDEX ... DD` collects `search_keys` (the presence owners) before dropping the index and then deletes
  each key with `Database::del`.

## Garbage collection

`IndexGcFilter` in [`rocks_storage.cpp`](../../src/storage/rocks_storage.cpp) runs during compactions of the
`index` column family and calls `IndexRegistry::owner_of`:

- the GC is paused (`IndexRegistry::pause_gc`, during a replica's full sync, when entries arrive before their
  definitions): keep the entry;
- the index id is not registered (dropped): drop the entry;
- the entry does not split: keep it;
- otherwise read the owner's meta record: drop the entry if it is missing, undecodable or expired, if a JSON
  index's owner is not `Json`, or if a hash index's owner is not a `Hash` with the entry's version.

Together with the in-batch maintenance this removes entries of deleted, expired, overwritten or renamed owners
and of dropped indexes. The filter does not compare the entry's tuple with the owner's current values; IDX
queries that read owners repair such entries (`StaleEntriesAreRepairedAndCompacted` in the tests forges them).
The data layout of all of this is in
[storage-format.md](storage-format.md#indexgcfilter-index).

## Known limitations and ideas

Current limitations, from the code:

- **No TEXT fields.** `FT.CREATE` rejects `TEXT`, `GEO`, `GEOSHAPE` and `VECTOR`, and the parser rejects bare
  words. A TEXT field could reuse the search layout with one entry per token (`id | field | term | owner`) plus a
  tokenizer and stemming, but scoring would need term statistics the index does not keep.
- **FT entries are trusted.** `SearchScanner` checks only the TTL in the entry; it never compares an entry with
  its owner and never repairs. Consistency relies on in-batch maintenance and on the compaction filter.
- **`within` does not save reads.** An intersection still scans each clause's whole range and filters by binary
  search. When `within` is small, probing `field_prefix | enc(v) | owner` with point reads (non-UNIQUE entries
  end with the owner) would read far less.
- **No cardinality estimates.** `And` orders TAG clauses first by a fixed rule, and `IDX.QUERY` always uses the
  one slice its index allows. Estimates (for example `GetApproximateSizes` over a clause's range, or counters per
  value) would let the planner start with the smallest clause.
- **Totals and FT.INFO cost a full scan.** `FT.SEARCH` always computes `total`, and `FT.INFO` scans every entry
  of the index. A document counter maintained with the presence entries (for example in the `System` column
  family, or with a merge operator) would make `num_docs` and `*` counts O(1).
- **Tuple cap.** A compound key with more than 1024 combinations is not indexed; a search key with more than
  1024 tuples keeps only the first 1024.
- **Backfill restarts from scratch** after a restart, and the keys it visits are loaded through `transact`
  (into the object cache).
- **Failed indexes keep their entries** until they are dropped; they are no longer maintained.
- **IDX planning** supports one range field after the equality prefix, no `OR`, and returns results only in index
  order.

## Tests

- [`tests/index_test.cpp`](../../tests/index_test.cpp): equality and range planning, maintenance on every write,
  backfill and restart, UNIQUE (including concurrent claims and rollback), drop, stale entry repair and
  compaction, pipelined writes.
- [`tests/search_test.cpp`](../../tests/search_test.cpp): definition encoding and `split_entry`, compound
  combinations, the query parser, FT queries on hashes and JSON, backfill, multi-valued compound indexes.
- [`tests/json_test.cpp`](../../tests/json_test.cpp): JSON documents, including JSON indexes.
