# JSON documents

datadam stores JSON documents under a key and serves them with the RedisJSON command set (`JSON.*`). Paths use
either JSONPath (`$...`) or the legacy RedisJSON syntax (`.a.b`), and the choice of syntax decides the shape of
each reply.

## Contents

- [Quick example](#quick-example)
- [Storage model](#storage-model)
- [Paths](#paths)
- [How the path syntax shapes replies](#how-the-path-syntax-shapes-replies)
- [Commands](#commands)
- [Limits and errors](#limits-and-errors)
- [Interaction with the rest of datadam](#interaction-with-the-rest-of-datadam)
- [Performance](#performance)
- [Not supported](#not-supported)

## Quick example

```text
127.0.0.1:6379> JSON.SET user:1 $ '{"name":"ana","tags":["a","b"],"address":{"city":"SP"}}'
OK
127.0.0.1:6379> JSON.GET user:1 $.address.city
"[\"SP\"]"
127.0.0.1:6379> JSON.GET user:1 .address.city
"\"SP\""
127.0.0.1:6379> JSON.SET user:1 $.age 30
OK
127.0.0.1:6379> JSON.NUMINCRBY user:1 $.age 1
"[31]"
127.0.0.1:6379> JSON.ARRAPPEND user:1 $.tags '"c"'
1) (integer) 3
127.0.0.1:6379> JSON.MERGE user:1 $ '{"address":null,"vip":true}'
OK
127.0.0.1:6379> JSON.GET user:1
"{\"name\":\"ana\",\"tags\":[\"a\",\"b\",\"c\"],\"age\":31,\"vip\":true}"
127.0.0.1:6379> TYPE user:1
ReJSON-RL
```

## Storage model

- A JSON key is an ordinary record (like a string) whose type is JSON. `TYPE` answers `ReJSON-RL`, as Redis
  with RedisJSON does.
- The document is stored as compact JSON text (no spaces). Objects keep their members in insertion order, and
  integers stay integers (`1` stays `1`; `1.5` is a float).
- Every write command reads the document, parses it, changes it and stores it whole, under the key's lock. A
  write therefore costs time proportional to the document size, however small the change.
- `JSON.GET key` with no path and no formatting options returns the stored text without parsing it.
- Documents are values like any other: they go through the same object cache, write batching, change feed and
  replication as strings (see [Interaction with the rest of datadam](#interaction-with-the-rest-of-datadam)).

## Paths

Every command that takes a path accepts both syntaxes. A path that starts with `$` is JSONPath; anything else is
a legacy path. When a command's path argument is optional and omitted, the legacy root `.` is used.

### JSONPath

| Selector | Meaning |
|---|---|
| `$` | The root. |
| `.name` | Member `name` of an object. The name runs until the next `.`, `[` or `]`. |
| `['name']`, `["name"]` | Member by quoted name; a backslash escapes the next character. |
| `[0]`, `[-1]` | Array element by position; negative positions count from the end. |
| `.*`, `[*]` | Every member of an object or element of an array. |
| `..name`, `..[sel]`, `..*` | Descendants: applies the selector to the current node and everything below it. |
| `[start:end]`, `[start:end:step]` | Array slice. `end` is exclusive; omitted bounds mean the whole array; negative bounds count from the end; a negative step walks backwards (`[::-1]`); step `0` selects nothing. |
| `[0,2]`, `['a','b']` | Union: several selectors in one bracket, of any of the kinds above. |
| `[?(expr)]` | Filter: the children (array elements or member values) for which `expr` holds. |

Filter expressions:

| Element | Syntax |
|---|---|
| Current node | `@`, followed by path steps: `@.price`, `@['a b']`, `@[0]` |
| Document root | `$` inside a filter: `?(@.price > $.limits.max)` |
| Literals | numbers (`10`, `-1.5e3`), strings in single or double quotes, `true`, `false`, `null` |
| Comparison | `==`, `!=`, `<`, `<=`, `>`, `>=` |
| Logic | `&&`, `\|\|`, `!` and parentheses; `&&` binds tighter than `\|\|` |
| Existence | a path alone: `?(@.isbn)` holds when the path selects something |

Comparison rules:

- Two numbers compare numerically; two strings compare byte by byte.
- Any other pair supports only `==` and `!=`, by JSON equality; the ordering operators are false.
- When an operand path selects several values, the first one is used.
- When an operand path selects nothing: `==` holds only if both sides are missing, `!=` holds if exactly one
  is, and ordering operators are false.

```text
127.0.0.1:6379> JSON.GET store '$..book[?(@.price < 10)].title'
127.0.0.1:6379> JSON.GET store '$..book[?(@.category == "fiction" && @.price > 10)].author'
127.0.0.1:6379> JSON.GET store '$..book[?(!(@.price < 10))].author'
127.0.0.1:6379> JSON.GET store '$..book[?(@.isbn)].author'
127.0.0.1:6379> JSON.GET nums '$[?(@ > 2)]'
```

### Legacy paths

Legacy paths are `.` (the root), `.a.b`, `a.b` (the leading dot is optional), `.a[0]`, `["a"]`, `.*` and `[*]`.
Only member names, array indexes and `*` are allowed; descendants (`..`), slices and filters are rejected with
`ERR invalid path '<path>': only names, indexes and * in a legacy path`. A legacy path selects at most one
value: the first match in document order.

## How the path syntax shapes replies

- **JSONPath (`$`)**: the command acts on every match and replies with one result per match, in document order.
  Where a match has the wrong type for the command, its result is nil. A path that matches nothing gives an
  empty result (`[]` or an empty array), not an error.
- **Legacy**: the command acts on the first match and replies with that single result. A path that matches
  nothing is an error (`ERR Path '<path>' does not exist`), and a match of the wrong type is an error such as
  `ERR the value at the path is not an array`.

Results that are JSON values (`JSON.GET`, `JSON.NUMINCRBY`, `JSON.NUMMULTBY`) come back as a bulk string of JSON
text: with `$` it is a JSON array of the values (`"[31]"`), with a legacy path the value itself (`"31"`).
Results that are counts or lengths come back as a RESP array of integers (with `$`) or one integer (legacy).

## Commands

In the syntax lines, `path` is required unless shown in brackets. Values are JSON text: a string value must
be quoted JSON (`'"text"'`).

### JSON.SET

```text
JSON.SET key path value [NX | XX]
```

Sets the value at `path`.

- On a missing key, only the root (`$` or `.`) can be set; any other path fails with
  `ERR new objects must be created at the root`.
- At the root of an existing key, the document is replaced. The key keeps its TTL.
- Elsewhere, every match is replaced. If the last step of the path is a plain member name (`$.a.new`,
  `$.*.new`) and an object that should hold it lacks it, the member is created. Nothing else is created: if a
  parent is missing, or the last step is an index, slice or filter that matches nothing, nothing is set.
- `NX`: set only if the path matches nothing now (at the root: only if the key does not exist).
- `XX`: set only if the path already matches; existing matches are replaced and nothing is created.
- `NX` and `XX` together are a syntax error.

Reply: `OK`, or nil when nothing was set (an `NX`/`XX` condition failed, or the path could not be created).
Errors: `WRONGTYPE` if the key holds another type, `ERR invalid JSON`, nesting deeper than 128 levels.

### JSON.GET

```text
JSON.GET key [INDENT s] [NEWLINE s] [SPACE s] [NOESCAPE] [path ...]
```

- No path: the whole document. Without formatting options this is the stored text, returned without parsing.
- One path: with `$`, a JSON array of all matches (`[]` if none); with a legacy path, the first match, or an
  error if there is none.
- Several paths: a JSON object keyed by each path's text, each value built as above
  (`{"$.a":[2],"$..b":[null]}`). A legacy path that matches nothing fails the whole command.
- `INDENT`, `NEWLINE` and `SPACE` format the output: `INDENT` is written once per nesting level, `NEWLINE`
  after each element and `SPACE` after each `:`. They may come in any order, before the paths. `NOESCAPE` is
  accepted and ignored.
- A missing key replies nil.

```text
127.0.0.1:6379> JSON.GET doc INDENT "\t" NEWLINE "\n" SPACE " " $.nested
"[\n\t{\n\t\t\"a\": 4,\n\t\t\"b\": null\n\t}\n]"
```

### JSON.MGET

```text
JSON.MGET key [key ...] path
```

One reply per key: with `$`, a JSON array of the matches; with a legacy path, the first match or nil. A missing
key, or a key that is not JSON, gives nil (not an error).

### JSON.MSET

```text
JSON.MSET key path value [key path value ...]
```

Sets each triple like `JSON.SET` without options; a path that matches nothing and cannot be created is skipped
silently. All paths and values are parsed before anything is written. The keys are then written one after the
other: if one fails (`WRONGTYPE`, or a non-root path on a missing key), the command returns that error and the
triples before it stay written. Reply: `OK`.

### JSON.DEL and JSON.FORGET

```text
JSON.DEL key [path]
JSON.FORGET key [path]
```

Removes the matched values and replies with how many were removed. The root (or no path) removes the key and
replies 1. When matches are nested inside each other (`$..a` on `{"a":{"a":1}}`), the inner ones go with the
outer one and count once. Removing array elements shifts the later ones. A missing key replies 0.

### JSON.TYPE

```text
JSON.TYPE key [path]
```

Type names: `object`, `array`, `string`, `integer`, `number` (a float), `boolean`, `null`. With `$`: an array of
type names (empty for a missing key). Legacy: one type name, or nil if the key or path is missing.

### JSON.NUMINCRBY and JSON.NUMMULTBY

```text
JSON.NUMINCRBY key path number
JSON.NUMMULTBY key path number
```

Adds `number` to, or multiplies by it, every numeric match. Integer with integer stays an integer unless the
result overflows 64 bits, in which case it becomes a float. Reply: the new values as JSON text (`"[null,4,7]"`
with `$`, nil where the match is not a number; `"2.5"` with a legacy path).

Errors: `ERR the increment is not a number`; `ERR the result is not a finite number` (nothing is changed); with
a legacy path, `ERR the value at the path is not a number`; on a missing key,
`ERR could not perform this operation on a key that doesn't exist`.

### JSON.STRAPPEND and JSON.STRLEN

```text
JSON.STRAPPEND key [path] json-string
JSON.STRLEN key [path]
```

`STRAPPEND` appends to every string match; the value must be a JSON string (`'"z"'`), otherwise
`ERR the value to append must be a JSON string`. Both reply with the new length(s), in bytes, nil for
non-string matches with `$` (`ERR the value at the path is not a string` with a legacy path).
`STRAPPEND` on a missing key is an error; `STRLEN` on a missing key replies nil (legacy) or an empty array
(`$`).

### JSON.ARRAPPEND

```text
JSON.ARRAPPEND key path value [value ...]
```

Appends the values to every array match. Reply: the new length(s). The path is required.

### JSON.ARRINSERT

```text
JSON.ARRINSERT key path index value [value ...]
```

Inserts the values before position `index` (negative counts from the end; `index` equal to the length
appends). If the index is out of range for any match, nothing changes and the reply is
`ERR index out of bounds`. Reply: the new length(s).

### JSON.ARRLEN

```text
JSON.ARRLEN key [path]
```

Array length(s); nil for non-array matches with `$`. A missing key replies nil (legacy) or an empty array (`$`).

### JSON.ARRPOP

```text
JSON.ARRPOP key [path [index]]
```

Removes and returns the element at `index` (default `-1`, the last). An index past either end takes the nearest
end. Reply: the popped element as JSON text; nil for an empty array or a non-array match. With `$`, an array
of such replies.

### JSON.ARRTRIM

```text
JSON.ARRTRIM key path start stop
```

Keeps the elements from `start` to `stop`, both inclusive, like `LTRIM`; negative positions count from the end.
Reply: the new length(s).

### JSON.ARRINDEX

```text
JSON.ARRINDEX key path value [start [stop]]
```

The position of the first element equal to `value` (compared as JSON) within `[start, stop)`, or `-1`.
Negative bounds count from the end; `stop` 0 (the default) means the end of the array. A missing key is an
error.

### JSON.OBJKEYS and JSON.OBJLEN

```text
JSON.OBJKEYS key [path]
JSON.OBJLEN key [path]
```

The member names (in insertion order), or how many there are. With `$`, one reply per match (nil for
non-objects). With a legacy path, a non-object is `ERR the value at the path is not an object`. A missing key
replies nil (legacy) or an empty array (`$`).

### JSON.TOGGLE

```text
JSON.TOGGLE key path
```

Flips every boolean match. Reply with `$`: an array of `1` (now true), `0` (now false) or nil (not a boolean).
With a legacy path: the bulk string `true` or `false`, or `ERR the value at the path is not a boolean`.

### JSON.CLEAR

```text
JSON.CLEAR key [path]
```

Empties every object and array match and sets every numeric match to 0. Strings, booleans and null are left as
they are. Reply: how many values changed (containers that were not empty, numbers that were not 0). A missing
key replies 0.

### JSON.MERGE

```text
JSON.MERGE key path value
```

Applies `value` to every match as an RFC 7386 merge patch: members of the patch are set, and a member whose
patch value is `null` is removed. On a missing key the path must be the root, and the key is created from the
patch. Reply: `OK`. With `$`, a path that matches nothing changes nothing and still replies `OK`; with a legacy
path it is `ERR Path '<path>' does not exist`.

## Limits and errors

- **Depth.** Documents, and values written into them, nest at most 128 levels. Deeper input fails with
  `ERR JSON nests deeper than 128 levels`; a write whose result would nest deeper fails the same way and
  changes nothing.
- **Invalid JSON** in a value: `ERR invalid JSON`.
- **Invalid paths**: `ERR invalid path '<path>': <reason>`.
- **Wrong key type**: `WRONGTYPE Operation against a key holding the wrong kind of value`, both for `JSON.*` on
  a non-JSON key and for string commands such as `GET` on a JSON key.
- **Missing key** for commands that need one (`NUMINCRBY`, `NUMMULTBY`, `STRAPPEND`, `ARRAPPEND`, `ARRINSERT`,
  `ARRPOP`, `ARRTRIM`, `ARRINDEX`, `TOGGLE`): `ERR could not perform this operation on a key that doesn't exist`.

## Interaction with the rest of datadam

- **Key commands.** `DEL`, `UNLINK`, `EXISTS`, `EXPIRE` and the other TTL commands, `PERSIST`, `RENAME` and
  `TYPE` work on JSON keys as on any record. `JSON.*` writes keep the key's TTL.
- **Other writes.** `SET`, `MSET` and memcache writes replace a JSON key with a string. A collection command
  that writes a destination key (for example `SUNIONSTORE`) replaces it too. memcache `get` does not return
  JSON keys.
- **`SCAN ... TYPE`.** `SCAN 0 TYPE ReJSON-RL` returns only JSON keys. The type name is compared without
  regard to case, so `rejson-rl` works too.
- **Object cache.** A document is cached like a string value of the same size; documents larger than
  `cache-max-object-size` are read from RocksDB. The reads `JSON.GET`, `JSON.MGET`, `JSON.TYPE`, `JSON.STRLEN`,
  `JSON.ARRLEN`, `JSON.ARRINDEX`, `JSON.OBJKEYS` and `JSON.OBJLEN` that miss the cache are handed to the read
  pool, like `GET` (see [operations.md](operations.md)).
- **Change feed.** Every write of a document appears in `CDC.READ` as a `json` change carrying the whole new
  document and its expiry time (see [change-feed.md](change-feed.md)).
- **Replication.** Documents replicate like any other write (see [replication.md](replication.md)).
- **Secondary indexes and search.** `IDX.CREATE ... ON JSON` and `FT.CREATE ... ON JSON` index values inside
  documents and are kept current by every write (see [secondary-indexes.md](secondary-indexes.md) and
  [search.md](search.md)).
- **ACL.** The commands are in the `@json` category, plus `@read` or `@write` and `@slow`. `JSON.MGET` and
  `JSON.MSET` check every key they name.

## Performance

Measured with a document of about 40 bytes and 100 connections:

| Command | thousand ops/s |
|---|---|
| `JSON.SET` | 292 (`SET` of the same text: 312) |
| `JSON.GET key` (whole document) | 470, the same as `GET` |
| `JSON.GET key $.age` | 451 |
| `JSON.NUMINCRBY` | 304 |

Large documents cost proportionally more on every write, because each write parses and stores the whole
document. Prefer several smaller keys to one large document that changes often.

## Not supported

- `JSON.RESP`, `JSON.DEBUG` and any other `JSON.*` command not listed above.
- Regular-expression filters (`=~`) and filter functions; a filter with `=~` is rejected as an invalid path.
- `JSON.ARRAPPEND` without a path (the path is required).
- `JSON.MSET` is not atomic across keys (see [JSON.MSET](#jsonmset)).
