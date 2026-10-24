# Search (FT.*)

datadam implements the RediSearch `FT.*` commands for `TAG` and `NUMERIC` fields over hashes and JSON documents,
so clients that already speak that syntax can query datadam without changes. There is no full-text search: no
`TEXT`, `GEO` or `VECTOR` fields.

## Contents

- [Quick example](#quick-example)
- [How a search index works](#how-a-search-index-works)
- [FT.CREATE](#ftcreate)
- [Query syntax](#query-syntax)
- [FT.SEARCH](#ftsearch)
- [Result order](#result-order)
- [FT.INFO](#ftinfo)
- [FT._LIST, FT.TAGVALS and FT.DROPINDEX](#ft_list-fttagvals-and-ftdropindex)
- [Querying during backfill](#querying-during-backfill)
- [Permissions](#permissions)
- [Differences from RediSearch](#differences-from-redisearch)
- [Costs](#costs)
- [Using existing clients](#using-existing-clients)

## Quick example

```text
FT.CREATE items ON HASH PREFIX 1 item: SCHEMA color TAG price NUMERIC SORTABLE size AS sz TAG CASESENSITIVE
FT.CREATE docs ON JSON PREFIX 1 doc: SCHEMA $.tags[*] AS tag TAG $.score AS score NUMERIC

FT.SEARCH items "@color:{red | blue} @price:[10 (50]" SORTBY price DESC LIMIT 0 10
FT.SEARCH items "@color:{gr*} -@sz:{M}" NOCONTENT          # prefix, negation
FT.SEARCH items "@price:[$lo $hi]" PARAMS 4 lo 10 hi 20 DIALECT 2
FT.SEARCH docs "*" RETURN 3 $.name AS name score LIMIT 0 0  # LIMIT 0 0: only the total
FT.INFO items
FT._LIST
FT.TAGVALS items color
FT.DROPINDEX items DD
```

## How a search index works

- Every hash (or, with `ON JSON`, every JSON document) whose key starts with one of the prefixes is a document of
  the index, even if it has none of the fields. Without `PREFIX`, every key of that type is covered.
- Each field is indexed on its own: one entry per value of each field, plus one presence entry per document.
  A document missing a field stays in the index; it is found by `*` and by queries on its other fields.
- A query is evaluated as set operations (intersection, union, difference) on the keys that index scans find.
  No document is read to evaluate the query; only the documents of the returned page are read, from a RocksDB
  snapshot.
- Entries are written in the same write batch as the data, carry the key's TTL, are built for existing keys by a
  background backfill, and are cleaned up by the same compaction filter as `IDX.*` indexes (see
  [secondary-indexes.md](secondary-indexes.md#consistency) and
  [dev/indexes-internals.md](dev/indexes-internals.md)).
- `FT.*` and `IDX.*` indexes share one name space and can cover the same keys.

## FT.CREATE

```text
FT.CREATE index [ON HASH | ON JSON] [PREFIX count prefix ...] [SKIPINITIALSCAN] [ignored options]
    SCHEMA field [AS name] type [field options] [field [AS name] type [field options] ...]

type:           TAG | NUMERIC
field options:  SEPARATOR c | CASESENSITIVE | SORTABLE | UNF | NOINDEX | WITHSUFFIXTRIE | INDEXEMPTY | INDEXMISSING
```

Options before `SCHEMA`, in any order:

| Option | Behavior |
|---|---|
| `ON HASH` / `ON JSON` | Source type. Default `HASH`. |
| `PREFIX count prefix ...` | Key prefixes covered. |
| `SKIPINITIALSCAN` | Skip the backfill: only keys written from now on are indexed, and the index is ready at once. |
| `STOPWORDS count word ...` | Accepted and ignored. |
| `LANGUAGE x`, `LANGUAGE_FIELD x`, `SCORE x`, `SCORE_FIELD x`, `PAYLOAD_FIELD x` | Accepted and ignored. |
| `MAXTEXTFIELDS`, `NOOFFSETS`, `NOHL`, `NOFIELDS`, `NOFREQS` | Accepted and ignored. |
| `FILTER`, `TEMPORARY` | Rejected: `ERR FILTER is not supported` (or `TEMPORARY`). |
| anything else | Rejected: `ERR unknown argument 'X'`. |

Fields:

- **Hash.** `field` is the hash field. `AS name` gives it another name for queries and `RETURN`.
- **JSON.** `field` is a JSONPath or legacy path (see [json.md](json.md#paths)). Without `AS`, the name used in
  queries is the path text, so give JSON fields an `AS` name.
- **`TAG`** values are case-insensitive by default: stored and queried lowercased (ASCII letters only).
  `CASESENSITIVE` keeps them as they are. Returned content is always the original.
- **`TAG` separator.** On a hash, a value is split at `,` by default (pieces trimmed of spaces and tabs, empty
  pieces dropped); `SEPARATOR c` sets another single character. On JSON there is no default separator: an array
  gives one value per element, and so does a path that matches several values. `SEPARATOR` on a JSON field
  splits string values.
- **`NUMERIC`** values must parse as numbers; other values are not indexed. A JSON array of numbers gives one
  value per element. Booleans are not numbers.
- `SORTABLE`, `UNF`, `NOINDEX`, `WITHSUFFIXTRIE`, `INDEXEMPTY` and `INDEXMISSING` are accepted and ignored:
  every field can be sorted on already, and `NOINDEX` fields are still indexed.
- JSON values follow the rules of `IDX.*` indexes: strings as they are, numbers and booleans as their JSON
  text, `null` and objects ignored.
- An index has at most 128 fields. A document keeps at most 1024 entries per index (its presence entry plus
  1023 values); further values are not indexed.

Errors: `ERR Index already exists`, `ERR ON takes HASH or JSON`, `ERR bad PREFIX count`,
`ERR bad STOPWORDS count`, `ERR expected SCHEMA`, `ERR field 'x' has no type`,
`ERR TEXT fields are not supported (only TAG and NUMERIC)` (also for `GEO`, `VECTOR`, `GEOSHAPE`),
`ERR unknown field type 'X'`, `ERR SEPARATOR takes a single character`, `ERR duplicate field 'x'`,
`ERR SCHEMA needs at least one field`, `ERR an index has at most 128 fields`, `ERR invalid path ...`.

## Query syntax

```text
query    := clause { clause }                 juxtaposition: AND (intersection)
clause   := unary { "|" unary }               union: binds tighter than AND
unary    := "-" unary                         NOT (all documents minus the clause)
          | "~" unary                         accepted; the clause is required, not optional
          | atom
atom     := "(" query ")"
          | "*"                               every document of the index
          | "@" name ":" "{" tags "}"         TAG field
          | "@" name ":" "[" bound [","] bound "]"   NUMERIC field
tags     := tag { "|" tag }
tag      := text | text "*" | $param          "*" at the end: prefix match
bound    := ["("] number | ["("] "-inf" | ["("] "+inf" | "inf" | $param
```

Because `|` binds tighter than juxtaposition, `@a:{x} @b:{y} | @c:{z}` means `@a:{x} AND (@b:{y} OR @c:{z})`.
Use parentheses to group otherwise.

**TAG clauses**

- `@color:{red}` matches documents with the value `red`; `@color:{red | blue}` either value.
- `@color:{gr*}` matches values that start with `gr`. Only a trailing, unescaped `*` is a prefix operator;
  `*abc` is an exact value.
- Spaces inside a value are kept; spaces around it are trimmed.
- A backslash escapes the next character: `@tag:{x\-y}` matches `x-y`.
- Double quotes make the text literal: `@tag:{"q|r"}` matches `q|r`.
- `$name` at the start of a value is replaced by the `PARAMS` value, literally (a `*` in it is not a prefix
  operator).
- Values are lowercased for matching unless the field is `CASESENSITIVE`.

**NUMERIC clauses**

- `@price:[10 50]` is inclusive at both ends. `(` before a bound excludes it: `@price:[(10 (50]`.
- `-inf`, `+inf` and `inf` (any case) are open ends: `@price:[100 +inf]`.
- A comma between the bounds is allowed: `@price:[10,50]`.
- A bound may be `$name`, replaced by the `PARAMS` value (which may itself start with `(`).

**Names** after `@` are the field names (the `AS` name if one was given). A backslash escapes characters in
the name.

**Errors** have the form `ERR Syntax error at offset N: <reason>`, for example:

| Query | Reason |
|---|---|
| `hello` | datadam has no full-text search: query TAG fields with @field:{value} and NUMERIC fields with @field:[min max] |
| `@nope:{x}` | unknown field 'nope' |
| `@price:{x}` | field 'price' is NUMERIC: use @price:[min max] |
| `@color:[1 2]` | field 'color' is TAG: use @color:{value} |
| `@color:{}` | empty tag |
| `@price:[1 x]` | bad upper bound 'x' |
| `@color:{$missing}` | no such parameter 'missing' (give it with PARAMS) |
| `(@price:[1 2]` | expected ')' |
| empty query | empty query |

## FT.SEARCH

```text
FT.SEARCH index query [NOCONTENT] [WITHSCORES] [FILTER field min max ...]
    [RETURN count name [AS alias] ...] [SORTBY field [ASC | DESC] [WITHCOUNT]]
    [LIMIT offset count] [PARAMS count name value ...] [ignored options]
```

| Option | Behavior |
|---|---|
| `NOCONTENT` | Return keys only. |
| `WITHSCORES` | Add a score after each key; it is always `1`. |
| `FILTER field min max` | Same as adding `@field:[min max]` to the query (the old numeric filter form). May be repeated. |
| `RETURN count name [AS alias] ...` | Return only these fields; `count` counts every argument, including `AS` and the alias. `RETURN 0` returns keys only. |
| `SORTBY field [ASC \| DESC]` | Order by an index field (`@field` also accepted). `WITHCOUNT` after it is accepted. Unknown field: `ERR Property 'x' not loaded nor in schema`. |
| `LIMIT offset count` | Default `LIMIT 0 10`. `LIMIT 0 0` returns only the total. Negative values: `ERR bad LIMIT`. |
| `PARAMS count name value ...` | Values for `$name` in the query. `count` must be even. |
| `VERBATIM`, `NOSTOPWORDS`, `INORDER`, `EXPLAINSCORE` | Accepted and ignored. |
| `DIALECT n`, `TIMEOUT n`, `SLOP n`, `LANGUAGE x`, `SCORER x`, `EXPANDER x` | Accepted and ignored. |
| anything else | `ERR unknown or unsupported argument 'X'` |

**RETURN names.** On a hash, a name is a field name of the index (mapped to its hash field) or any hash field. On
JSON, a name is an index field (its path is used), a path (`$.name`, `.name`), or a bare name meaning the
top-level member. A JSON string is returned as it is, any other value as JSON text, and only the first value a
path selects. Names a document does not have are left out of its entry.

**Reply** (the RESP2 shape, also on RESP3 connections):

```text
[total, key1, [field, value, ...], key2, [field, value, ...], ...]
```

- `total` is the number of matching documents, before `LIMIT`.
- Without `RETURN`, a hash entry lists every field of the hash, and a JSON entry is `["$", document]`.
- With `NOCONTENT` (or `RETURN 0`) the field arrays are omitted: `[total, key1, key2, ...]`.
- With `WITHSCORES`, the score `"1"` follows each key.
- A document deleted between the query and the read of its content is left out of the page.

```text
127.0.0.1:6379> FT.SEARCH items "@price:[40 40]"
1) (integer) 1
2) "item:3"
3) 1) "color"
   2) "red"
   3) "price"
   4) "40"
127.0.0.1:6379> FT.SEARCH items "@sz:{M} @price:[20 30]" RETURN 3 price AS p
1) (integer) 1
2) "item:2"
3) 1) "p"
   2) "25.5"
127.0.0.1:6379> FT.SEARCH items "*" LIMIT 0 0
1) (integer) 3
```

Errors: `ERR <index>: no such index` (also when the name is an `IDX.*` index), `ERR index 'x' failed: ...`,
`ERR bad RETURN count`, `ERR bad RETURN: AS needs a name`, `ERR bad PARAMS count`, `ERR bad LIMIT`, query
syntax errors.

## Result order

- **Without `SORTBY`**: by key, in byte order.
- **With `SORTBY`**: by the field's index entries, ascending or descending. Documents with equal values come in
  key order (reversed with `DESC`). Documents without the field come last, in key order. A multi-valued field
  sorts a document by its first value in that direction: its smallest value ascending, its largest descending.
  The scan stops as soon as `offset + count` documents are found.
- The total is always computed exactly, whatever the `LIMIT`.

## FT.INFO

```text
FT.INFO index
```

A map (a flat array in RESP2) with these fields:

| Field | Value |
|---|---|
| `index_name` | the name |
| `index_options` | always an empty array |
| `index_definition` | `[key_type, HASH or JSON, prefixes, [prefix ...], default_score, "1"]`; no prefix shows as `[""]` |
| `attributes` | per field: `[identifier, path or hash field, attribute, name, type, TAG or NUMERIC]`, and for `TAG` also `SEPARATOR, c` (empty when none) and `CASESENSITIVE` when set |
| `num_docs` | documents in the index (live presence entries) |
| `num_records` | all live entries, presence entries included |
| `indexing` | 1 while the backfill runs, else 0 |
| `percent_indexed` | `"0"` while the backfill runs, else `"1"` |
| `hash_indexing_failures` | always 0 |
| `state` | `building`, `ready` or `failed` |

`num_docs` and `num_records` are counted by scanning the whole index, so `FT.INFO` costs time proportional to
the index size. `IDX.INFO` also works on search indexes (with `kind` `search`).

## FT._LIST, FT.TAGVALS and FT.DROPINDEX

```text
FT._LIST
FT.TAGVALS index field
FT.DROPINDEX index [DD]
```

- `FT._LIST` returns the names of the search indexes, sorted. (`IDX.LIST` returns all indexes.)
- `FT.TAGVALS` returns the distinct values of a `TAG` field as stored, that is lowercased unless
  `CASESENSITIVE`, in byte order. A field that is not `TAG` gives `ERR no TAG field 'x' in index 'y'`.
- `FT.DROPINDEX` removes the index and its entries. With `DD` it also deletes every document that was in the
  index, one `DEL` at a time after the index is gone (not atomically). Other trailing arguments are a syntax
  error.

## Querying during backfill

`FT.CREATE` registers the index, which every write maintains from then on, and indexes existing keys in the
background. Unlike `IDX.QUERY`, `FT.SEARCH` answers during the backfill with what has been indexed so far, as
RediSearch does; `FT.INFO` shows `indexing 1`. With `SKIPINITIALSCAN` there is no backfill and existing keys
stay out of the index until they are written again.

## Permissions

- `FT.CREATE` and `FT.DROPINDEX` are in `@write`, `@admin`, `@dangerous`, `@slow` and `@index` (`@search` is
  an alias). `FT.SEARCH`, `FT.INFO`, `FT._LIST` and `FT.TAGVALS` are in `@read`, `@slow` and `@index`.
- `FT.SEARCH` returns and counts only the keys the user may read. `FT.INFO` counts and `FT.TAGVALS` values are
  not filtered.
- `FT.DROPINDEX ... DD` deletes the documents without checking the user's key permissions; restrict it like
  other `@dangerous` commands.

See [security.md](security.md).

## Differences from RediSearch

- **Field types.** Only `TAG` and `NUMERIC`. `TEXT`, `GEO`, `GEOSHAPE` and `VECTOR` are rejected by
  `FT.CREATE`; bare words in a query are an error.
- **Commands.** Only `FT.CREATE`, `FT.SEARCH`, `FT.INFO`, `FT._LIST`, `FT.TAGVALS` and `FT.DROPINDEX`. There is
  no `FT.AGGREGATE`, `FT.ALTER`, `FT.EXPLAIN`, `FT.PROFILE`, aliases (`FT.ALIASADD`...), cursors, synonyms,
  dictionaries, suggestions or `FT.CONFIG`.
- **Scores.** No scoring: `WITHSCORES` always returns `1`, and `~` does not make a clause optional (the clause
  must match).
- **Order.** Without `SORTBY`, results are in key order, not by score or insertion.
- **Totals.** The total is always exact, and computing it reads every entry of every field in the query.
- **Case folding** of `TAG` values covers ASCII letters only.
- **Tag matching.** Exact values and prefixes (`abc*`); no suffix or infix patterns.
- **Ignored options.** `SORTABLE`, `UNF`, `NOINDEX`, `INDEXEMPTY`, `INDEXMISSING`, `WITHSUFFIXTRIE`, `DIALECT`,
  `TIMEOUT` and the others listed above are accepted without effect. There is no `ismissing()`/`isempty()`
  query function.
- **Unsupported `FT.SEARCH` options** such as `INKEYS`, `INFIELDS`, `HIGHLIGHT`, `SUMMARIZE`, `GEOFILTER`,
  `WITHPAYLOADS` and `WITHSORTKEYS` return an error.
- **`FT.CREATE FILTER`** expressions and `TEMPORARY` indexes are not supported.
- **Reply shape.** `FT.SEARCH` always uses the RESP2 array shape, also on RESP3 connections.
- **`FT.INFO`** has only the fields listed above.
- **Name space.** `FT.*` and `IDX.*` indexes share names; an `IDX.*` index is not visible to `FT.*` commands.
- **Key permissions.** Results are filtered by the user's readable keys.

## Costs

Measured in Docker with redis-benchmark and 50 connections; the queries ran over 100 000 hashes with 100
colors, 1 000 prices and 10 sizes.

| Operation | thousand ops/s |
|---|---|
| `HSET` of 3 fields with new values: no index / `FT` index of 3 fields / `IDX` compound of 2 | 222 / 111 / 100 |
| `FT.SEARCH "@color:{c42}" LIMIT 0 10` (1 000 results, returns 10 hashes) | 33 |
| the same with `NOCONTENT` | 40 |
| `FT.SEARCH "@color:{c42} @price:[0 99]"` (intersection of 1 000 with 10 000) | 3.9 |
| `FT.SEARCH "@price:[0 99]" SORTBY price LIMIT 0 10` (10 000 results) | 3.2 |
| `FT.SEARCH "*" LIMIT 0 0` (counts 100 000) | 0.9 |
| `IDX.QUERY color = c42 price < 100 LIMIT 0 10` on a compound index | 80 |

Because `FT.SEARCH` always computes the total, it pays for every entry of every field in the query. For a
combination of fields that is queried all the time, a compound `IDX.*` index over the same data reads only the
slice of the page and was 20 times faster in this test (see
[secondary-indexes.md](secondary-indexes.md#idx-or-ft)).

## Using existing clients

Clients send `FT.*` commands as plain Redis commands, so a RediSearch client library works as long as it uses the
supported subset. Two points to check:

- Use a RESP2 connection for `FT.SEARCH`. Client libraries that expect RediSearch's RESP3 map reply on a RESP3
  connection will not parse datadam's array reply.
- Options outside the subset (for example `HIGHLIGHT` or `INKEYS`) fail with
  `ERR unknown or unsupported argument`.

The commands a typical client sends, written out:

```text
FT.CREATE idx:products ON HASH PREFIX 1 product: SCHEMA category TAG brand TAG price NUMERIC SORTABLE
HSET product:1 category "Shoes" brand "acme" price 59.9
FT.SEARCH idx:products "@category:{shoes} @price:[(0 100]" SORTBY price ASC LIMIT 0 20 DIALECT 2
FT.SEARCH idx:products "@brand:{$b}" PARAMS 2 b acme RETURN 2 price category NOCONTENT
```

The same from Python with redis-py's generic command interface:

```python
import redis

r = redis.Redis(host="localhost", port=6379, protocol=2, decode_responses=True)
r.execute_command(
    "FT.CREATE", "idx:products", "ON", "HASH", "PREFIX", "1", "product:",
    "SCHEMA", "category", "TAG", "brand", "TAG", "price", "NUMERIC", "SORTABLE",
)
r.hset("product:1", mapping={"category": "Shoes", "brand": "acme", "price": 59.9})

reply = r.execute_command(
    "FT.SEARCH", "idx:products", "@category:{shoes} @price:[(0 100]",
    "SORTBY", "price", "ASC", "LIMIT", 0, 20,
)
total, rest = reply[0], reply[1:]
for key, fields in zip(rest[0::2], rest[1::2]):
    doc = dict(zip(fields[0::2], fields[1::2]))
    print(key, doc)
```
