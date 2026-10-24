# Change feed (CDC)

When the RocksDB write-ahead log (WAL) is retained, `CDC.READ` returns, in commit order, what each write batch
changed: keys set and deleted, collection elements added and removed, JSON documents written. Consumers use it to
keep caches, search engines or other databases in sync with datadam.

## Contents

- [Enabling the feed](#enabling-the-feed)
- [CDC.READ](#cdcread)
- [Reply format](#reply-format)
- [Change operations](#change-operations)
- [Cursor semantics](#cursor-semantics)
- [What is not in the feed](#what-is-not-in-the-feed)
- [CDC.INFO](#cdcinfo)
- [Permissions](#permissions)
- [Writing a consumer](#writing-a-consumer)
- [Cost](#cost)

## Enabling the feed

The feed is off by default. Set these options in the configuration file (or on the command line) and restart;
they cannot be changed with `CONFIG SET`:

```text
wal-retention 24h          # keep WAL files this long after RocksDB no longer needs them; 0 = feed off
wal-retention-size 1gb     # cap on the kept WAL (default 1gb)
```

- `wal-retention` is a duration: a number of seconds, or a number with `ms`, `s`, `m`/`min`, `h` or `d`. It must
  be 0 or at least 1 s.
- The feed also needs the WAL itself: `experimental-write-path` must be `wal` (the default) or `wal-buffered`.
  With `no-wal` or `memory`, the feed stays off.
- Kept WAL files are deleted when they are older than `wal-retention`, and the oldest ones first whenever the
  kept files exceed `wal-retention-size`. RocksDB checks its own size limit only every 10 minutes, so datadam
  enforces `wal-retention-size` itself once per second. With `wal-retention-size 0`, only the age limit
  applies.
- The kept WAL is as large as what was written during the retention period; size the disk accordingly.
- The same retained WAL feeds replication (see [replication.md](replication.md)).

When the feed is off, `CDC.READ` fails with
`ERR the change feed is off: set wal-retention (it also needs experimental-write-path wal or wal-buffered)`.

See [configuration.md](configuration.md) for all options.

## CDC.READ

```text
CDC.READ <sequence | $> [COUNT n] [MATCH pattern] [BLOCK ms]
```

| Argument | Meaning |
|---|---|
| `sequence` | Read from this WAL sequence number. `0` means the oldest batch still kept. |
| `$` | Only batches written after this call. |
| `COUNT n` | Stop once at least `n` changes were collected (default 100). Batches are never split, so a reply may hold more than `n` changes. Must be at least 1. |
| `MATCH pattern` | Only changes to keys matching this glob pattern (as in `SCAN`). |
| `BLOCK ms` | When there is nothing to return, wait up to `ms` milliseconds for a new write; `0` waits without limit. |

- A read looks at no more than 10 000 batches. When `MATCH` filters out most of them, the reply may be empty
  while the returned sequence still moves forward; keep reading from it.
- `BLOCK` works like `XREAD BLOCK`: the connection waits without holding an I/O thread, and wakes on the next
  committed write. If that write does not match (`MATCH`, key permissions), it keeps waiting until the timeout.
  On timeout, the reply has no batches. `$` is resolved once, on the first attempt, so a write that lands
  between the call and the wait is not missed.
- `BLOCK` applies on Redis protocol connections; where a connection cannot wait, the command returns at once.

Errors: `ERR invalid sequence: a number, or $ for new changes only`, `ERR COUNT must be a positive integer`,
`ERR timeout is not an integer or out of range`, `ERR syntax error` (an unknown option or a missing value),
`ERR sequence N is no longer in the kept WAL (the oldest is M)`.

## Reply format

```text
[next-sequence, [batch, batch, ...]]
batch  = [sequence, commit-time-ms, [change, change, ...]]
change = [op, key, arg, ...]
```

- `next-sequence` is where to continue: pass it to the next `CDC.READ`.
- `sequence` is the WAL sequence number of the batch's first write.
- `commit-time-ms` is the commit time in Unix milliseconds (0 if the batch carries no time).
- Every element is a bulk string except the sequence numbers and the commit time, which are integers.
- Batches whose changes were all filtered out (`MATCH`, permissions, or only index entries) are omitted.

```text
127.0.0.1:6379> CDC.READ 1234 COUNT 100
1) (integer) 1238
2) 1) 1) (integer) 1234
      2) (integer) 1761170000123
      3) 1) 1) "set"
            2) "user:1"
            3) "ana"
            4) "0"
   2) 1) (integer) 1235
      2) (integer) 1761170000150
      3) 1) 1) "meta"
            2) "user:2"
            3) "hash"
            4) "2"
            5) "0"
            6) "7"
         2) 1) "hset"
            2) "user:2"
            3) "city"
            4) "SP"
         3) 1) "hset"
            2) "user:2"
            3) "age"
            4) "30"
```

## Change operations

Times (`expire_at_ms`) are Unix milliseconds, `0` for no TTL.

**Records**

| Op | Arguments | Meaning |
|---|---|---|
| `set` | `key value expire_at_ms` | A string was written (`SET`, `INCR`, `APPEND`, `EXPIRE` on a string, a memcache write...). |
| `json` | `key document expire_at_ms` | A JSON document was written; `document` is the whole new document as compact JSON. |
| `del` | `key` | The key was removed (`DEL`, a lazy expiry on read, the last element of a collection removed, the old name in `RENAME`...). |
| `meta` | `key type size expire_at_ms version [head]` | A collection's record was written: created, resized, or its TTL changed. `type` is `hash`, `set`, `zset` or `list`; `head` appears only for lists. A new `version` means the old contents are gone. |
| `flushall` | `""` (empty key) | Every key was removed. A batch with `flushall` contains only that change. |

**Collection elements**

| Op | Arguments | Meaning |
|---|---|---|
| `hset` | `key field value` | A hash field was set. |
| `hdel` | `key field` | A hash field was removed. |
| `sadd` | `key member` | A set member was added. |
| `srem` | `key member` | A set member was removed. |
| `zadd` | `key score member` | A sorted-set member was added or its score changed. |
| `zrem` | `key member` | A sorted-set member was removed. |
| `lset` | `key position value` | A list slot was written. |
| `ldel` | `key position` | A list slot was removed. |
| `data` | `key version sub-key value` | An element whose collection type is unknown (fallback, see below). |
| `datadel` | `key version sub-key` | Removal of such an element. |

Notes:

- **List positions.** `position - head` is the element's index in the list, using the `head` of the latest
  `meta` for that key. Positions can be negative: an `LPUSH` writes below the old head.
- **Order inside a batch.** Record changes (`set`, `json`, `del`, `meta`) come first, in write order, followed
  by the element changes of the batch.
- **Replaced collections.** Elements written under a collection version that the same batch deletes are not
  reported. A collection emptied by its last `SREM`/`HDEL`/... shows as `del` only.
- **`data`/`datadel`** appear only when a batch carries no type information for that collection; normal writes
  always carry it.

The test suite pins down sequences like this one (versions omitted):

```text
SET s v                      -> set s v 0
HSET h f1 v1 f2 v2           -> meta h hash 2 0 <v>, hset h f1 v1, hset h f2 v2
HDEL h f1                    -> meta h hash 1 0 <v>, hdel h f1
SADD st m1 ; SREM st m1      -> meta st set 1 0 <v>, sadd st m1 ; del st
RPUSH l x y                  -> meta l list 2 0 <v> 0, lset l 0 x, lset l 1 y
LPUSH l w                    -> meta l list 3 0 <v> -1, lset l -1 w
RENAME h h2                  -> meta h2 hash 1 0 <v>, del h, hset h2 f2 v2
JSON.SET doc $ '{"a":1}'     -> json doc {"a":1} 0
FLUSHALL                     -> flushall ""
```

## Cursor semantics

- Each batch is an atomic unit: it is returned whole, with all its changes, or not at all. `COUNT` is reached
  with whole batches.
- `next-sequence` follows the last batch examined, including batches that were filtered out. With nothing to
  return it stays where it was (for `$`, it is the sequence after the latest write).
- A sequence older than the oldest kept WAL fails with
  `ERR sequence N is no longer in the kept WAL (the oldest is M)`. The consumer has fallen behind the retention
  and must rebuild its state from a snapshot of the data (see [Writing a consumer](#writing-a-consumer)).
- Sequence numbers belong to one server's RocksDB. A replica has its own WAL and its own numbers; a cursor from
  the primary is not valid on a replica.
- The feed survives restarts: retained WAL files stay on disk.

## What is not in the feed

- **Index entries** of `IDX.*` and `FT.*` indexes, which are derived from the data; also index definitions and
  other internal records.
- **Expirations made by compaction.** A key whose TTL passes is removed by RocksDB compaction without a WAL
  record, so no `del` appears. That is why `set`, `json` and `meta` carry `expire_at_ms`: a consumer must treat
  a key as gone at that time. A key that expires and is then read on the primary is deleted lazily, and that
  deletion does appear as `del`.
- **Element changes of a collection version deleted in the same batch** (see above).

## CDC.INFO

```text
CDC.INFO
```

A map (a flat array in RESP2):

| Field | Value |
|---|---|
| `enabled` | `yes` or `no` |
| `wal_retention_seconds` | configured `wal-retention`, in seconds |
| `wal_retention_size` | configured `wal-retention-size`, in bytes |
| `first_sequence` | first sequence of the oldest kept WAL file (0 when the feed is off) |
| `last_sequence` | latest sequence written |
| `wal_bytes` | size of the WAL files on disk, live and kept |

## Permissions

- `CDC.READ` is in `@admin`, `@dangerous` and `@slow`, because it shows the value of any key. `CDC.INFO` is in
  `@admin` and `@slow`.
- For a user with key patterns, `CDC.READ` returns only changes to keys the user may read. `flushall` is always
  returned.

```text
ACL SETUSER feed on >secret ~orders:* +cdc.read +cdc.info
```

See [security.md](security.md).

## Writing a consumer

A consumer keeps its cursor durably together with what it applied, so a restart resumes exactly where it
stopped. Applying a change must be idempotent: after a crash between applying a batch and saving the cursor,
the batch is delivered again.

To start from scratch or recover from `no longer in the kept WAL`:

1. Read `last_sequence` from `CDC.INFO`.
2. Copy the current data (for example with `SCAN` and the type-specific reads).
3. Continue with `CDC.READ <last_sequence + 1>`. Changes made during the copy are replayed; because they are
   idempotent upserts and deletes, the result converges.

```python
import redis

r = redis.Redis(host="localhost", port=6379, decode_responses=True)


def load_cursor():
    ...  # from the consumer's own durable storage; None the first time


def save_cursor(seq):
    ...


def full_resync():
    info = r.execute_command("CDC.INFO")
    info = dict(zip(info[0::2], info[1::2]))
    start = int(info["last_sequence"]) + 1
    for key in r.scan_iter(match="orders:*", count=1000):
        ...  # copy the key into the target
    return start


def apply(op, key, args, commit_ms):
    if op in ("set", "json"):
        value, expire_at_ms = args[0], int(args[1])
        ...  # upsert; drop it at expire_at_ms if non-zero
    elif op == "del":
        ...  # delete
    elif op == "hset":
        ...  # set field args[0] to args[1]
    elif op == "hdel":
        ...
    elif op == "meta":
        ...  # args: type, size, expire_at_ms, version[, head]; if the version changed, clear the old contents
    elif op == "flushall":
        ...  # drop everything


cursor = load_cursor()
if cursor is None:
    cursor = full_resync()

while True:
    try:
        reply = r.execute_command("CDC.READ", cursor, "COUNT", 500, "MATCH", "orders:*", "BLOCK", 5000)
    except redis.ResponseError as e:
        if "no longer in the kept WAL" in str(e):
            cursor = full_resync()
            continue
        raise
    next_seq, batches = reply
    for seq, commit_ms, changes in batches:
        for op, key, *args in changes:
            apply(op, key, args, commit_ms)
    cursor = next_seq
    save_cursor(cursor)
```

Use a client timeout longer than `BLOCK`, or the client may give up before the server answers.

## Cost

With the feed on, each write batch carries a small note (commit time and the type of each collection it
touches) and the WAL files are kept instead of deleted. In a pipelined pure-write benchmark (1 million keys, 100
connections), throughput was 568 to 570 thousand writes/s with the feed on against 607 to 625 thousand with it
off, about 7% less. Reading the feed decodes WAL files from disk and does not touch the object cache.

Internals: [dev/replication-and-cdc-internals.md](dev/replication-and-cdc-internals.md).
