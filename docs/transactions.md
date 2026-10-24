# Transactions

`MULTI`, `EXEC`, `DISCARD`, `WATCH` and `UNWATCH` work as in Redis. A transaction queues commands and runs them
as one block that no other client can interleave with, and `WATCH` turns it into an optimistic
check-and-set.

## Contents

- [Queueing and running](#queueing-and-running)
- [Errors](#errors)
- [WATCH](#watch)
- [What the block guarantees](#what-the-block-guarantees)
- [Commands inside a block](#commands-inside-a-block)
- [Example: a check-and-set](#example-a-check-and-set)

## Queueing and running

```text
> MULTI
OK
> INCR counter
QUEUED
> LPUSH log "incremented"
QUEUED
> EXEC
1) (integer) 1
2) (integer) 1
```

After `MULTI`, every command is checked (it must exist, have the right number of arguments, and be allowed
to the ACL user, keys included) and queued, and the reply is `QUEUED`. Nothing runs until `EXEC`, which
replies with an array holding the reply of each queued command, in order. `DISCARD` drops the queue.

`EXEC`, `DISCARD`, `MULTI`, `WATCH` and `QUIT` are never queued.

## Errors

| Situation | Result |
|---|---|
| A command is refused while queueing (unknown, wrong arity, ACL) | The refusal is replied at once, and `EXEC` replies `EXECABORT Transaction discarded because of previous errors.` without running anything. |
| A command fails while the block runs (`WRONGTYPE`, a bad argument value) | Its slot in the `EXEC` array holds the error. The other commands run, and nothing is undone, as in Redis. |
| `MULTI` inside `MULTI` | `ERR MULTI calls can not be nested`; the transaction goes on. |
| `WATCH` inside `MULTI` | `ERR WATCH inside MULTI is not allowed`. |
| `EXEC` or `DISCARD` without `MULTI` | `ERR EXEC without MULTI` / `ERR DISCARD without MULTI`. |

## WATCH

`WATCH key [key ...]` makes the next `EXEC` fail, replying a nil array and running nothing, if any of those
keys is written after the `WATCH`. Any write counts, by any client, this one included: setting a string,
changing one element of a collection, deleting, expiring through `EXPIRE`, renaming, `FLUSHALL`, and writes
arriving from the primary on a replica. `EXEC`, `DISCARD` and `UNWATCH` forget the watched keys, and so does
closing the connection.

The check is exact: only writes to the watched keys make `EXEC` fail, never writes to other keys. A key that
expires on its own, without being read or written, does not count as written.

## What the block guarantees

`EXEC` first locks every key that its commands name, using the same per-key locks as all other writes. A
command that touches keys it does not name (`KEYS`, `SCAN`, `FLUSHALL`, `IDX.QUERY`, `FT.SEARCH`,
`CDC.READ`...) makes the block lock everything instead. Then:

- **Isolation from writers.** No other client writes any of those keys until the block ends. Two blocks never
  see each other half done.
- **Isolation from disk reads.** A read from another client that has to go to RocksDB waits for the block, so
  it sees all of it or none of it.
- **Reads inside the block** see the block's own earlier writes and nothing from other clients.
- **One commit.** The block's writes reach RocksDB in one atomic batch, so a crash keeps all of them or none.
  The exception is a block with a command that commits on its own (`MSET`, `MSETNX`, `RENAME`, `FLUSHALL`,
  `KEYS`, `SCAN`, `DBSIZE`, `IDX.QUERY`, `FT.SEARCH`, a write to a `UNIQUE` index): what is pending is written
  at that point, still under the block's locks, so other clients still see none of it early, but a crash in the
  middle of such a block can keep its first part.
- **No early peeks.** A read answered from the object cache without a lock never sees the block's writes
  before the block ends: they stay marked as pending in the cache until the block commits, and a reader that
  meets a pending item waits for the key's lock instead. So no client sees part of a block, whether it reads
  from the cache or from disk.

A block that locks everything stops every other writer while it runs, so keep such blocks short.

## Commands inside a block

- Blocking commands (`BLPOP`...) answer at once inside a block: nil when there is no data.
- Reads that miss the cache read the disk on the connection's own thread, instead of going through the read
  pool, because the block holds its locks.
- `SUBSCRIBE` and the other Pub/Sub commands can be queued; their confirmations arrive as part of `EXEC`.
- Commands work the same on a replica (only reads are allowed there).

## Example: a check-and-set

Increment a value only if nobody changed it meanwhile, retrying otherwise:

```python
import redis

r = redis.Redis(port=6379)
with r.pipeline() as pipe:
    while True:
        try:
            pipe.watch("balance")
            balance = int(pipe.get("balance") or 0)
            pipe.multi()
            pipe.set("balance", balance + 10)
            pipe.execute()
            break
        except redis.WatchError:
            continue  # someone wrote "balance" after WATCH: read it again
```

For the implementation (locks, the batch, and how `WATCH` is told about writes), see
[Concurrency](dev/concurrency.md#multiexec-blocks).
