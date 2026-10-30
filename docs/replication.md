# Replication

A datadam replica follows a primary over the Redis protocol and serves reads; writes go to the primary only.
Replication is asynchronous, built on the primary's retained RocksDB write-ahead log (WAL): a replica first copies
a consistent snapshot, then applies every write batch in order, and resumes from where it stopped after a
reconnect or restart.

## Contents

- [Quick start](#quick-start)
- [Configuration](#configuration)
- [Commands](#commands)
- [How a replica syncs](#how-a-replica-syncs)
- [Behavior on a replica](#behavior-on-a-replica)
- [Monitoring: ROLE and INFO replication](#monitoring-role-and-info-replication)
- [Promotion](#promotion)
- [WAIT](#wait)
- [Replication over TLS](#replication-over-tls)
- [Failover](#failover)
- [Limits](#limits)
- [Runbook](#runbook)

## Quick start

On the primary, retain the WAL (this needs a restart):

```text
# datadam.conf on the primary
wal-retention 1h
```

On the replica, either in its configuration file:

```text
replicaof 10.0.0.1 6379
```

or at run time:

```text
127.0.0.1:6380> REPLICAOF 10.0.0.1 6379
OK
127.0.0.1:6380> ROLE
1) "slave"
2) "10.0.0.1"
3) (integer) 6379
4) "connected"
5) (integer) 48213
```

## Configuration

| Option | Where | Meaning |
|---|---|---|
| `wal-retention` | primary | How long WAL files are kept after RocksDB no longer needs them. Must be greater than 0 for the primary to serve replicas, and long enough to cover a full sync plus any replica outage you want to bridge without a new full sync. Not changeable at run time. |
| `wal-retention-size` | primary | Cap on the kept WAL (default `1gb`). A replica that falls behind what is kept does a full sync. |
| `wal-retention-hold` | primary | How far past `wal-retention-size` the WAL a connected replica still needs is kept (default `8gb`): a full sync takes a while, and the replica continues from the WAL written meanwhile. |
| `experimental-write-path` | primary | Must be `wal` (default) or `wal-buffered`; without a WAL there is nothing to replicate. |
| `replicaof host port` | replica | Follow this primary from startup. `slaveof` is an alias. Set by `REPLICAOF` at run time and written by `CONFIG REWRITE`. |
| `masteruser` | replica | ACL user to log in to the primary with. Changeable with `CONFIG SET`. |
| `masterauth` | replica | Password for the primary. If set, the replica sends `AUTH masteruser masterauth` (or `AUTH masterauth` without `masteruser`) before anything else. Changeable with `CONFIG SET`; used on the next connection. |
| `protected-mode` | primary | With `yes` (the default outside Docker) and no password for the `default` user, a replica on another machine is refused (`DENIED`). Give the primary a password and the replica `masterauth`, or use `no` on a private network. See [Security](security.md#protected-mode). |
| `tls-replication` | replica | `yes`: connect to the primary with TLS (the `replicaof` port must then be its `tls-port`). Default `no`. See [Replication over TLS](#replication-over-tls). |
| `tls-client-cert-file`, `tls-client-key-file` | replica | The certificate the replica presents to the primary over TLS; empty: `tls-cert-file` / `tls-key-file`. |
| `replica-priority` | replica | What Redis Sentinel reads as `slave_priority`: among the replicas it may promote, the lowest wins; `0` never. Default `100`. |

The user the replica logs in with must be allowed to run `PING`, `REPLCONF` and `REPLSYNC`. `REPLCONF` and
`REPLSYNC` are in `@admin` and `@dangerous`, and touch no keys:

```text
ACL SETUSER replicator on >change-me +ping +replconf +replsync
```

With `promotion-handoff yes` the user also needs `+failover`: a hand-over makes the primary follow the
replica, as `FAILOVER TO` it would, so the primary only hands over to a replica whose user may run `FAILOVER`
(see [below](#promotion-with-a-hand-over)). Without it, the replica becomes a primary on its own after 5 s.

If a replica may be promoted and then serve replicas of its own, give it a `wal-retention` too: the option
only takes effect at startup.

See [configuration.md](configuration.md) and [security.md](security.md).

## Commands

| Command | Reply | Meaning |
|---|---|---|
| `REPLICAOF host port` | `OK` | Become a replica of `host:port` (or switch to another primary). The server turns read-only at once and connects in the background. Bad port: `ERR Invalid master port`. |
| `REPLICAOF NO ONE` | `OK` | Stop following and become a primary (see [Promotion](#promotion)). On a primary it does nothing. |
| `SLAVEOF ...` | | Alias of `REPLICAOF`. |
| `ROLE` | array | See [Monitoring](#monitoring-role-and-info-replication). |
| `INFO replication` | text | See [Monitoring](#monitoring-role-and-info-replication). |
| `WAIT numreplicas timeout` | integer | Waits until that many replicas have every write committed before it, or `timeout` milliseconds (0: no limit); replies how many have. See [WAIT](#wait). |
| `FAILOVER [TO host port [FORCE]] [TIMEOUT ms] [ABORT]` | `OK` | Hands the primary role over to a replica, without losing writes. See [Failover](#failover). |
| `REPLCONF`, `REPLSYNC` | | Used between servers; not for clients. |

`REPLICAOF` and `SLAVEOF` are in `@admin`, `@dangerous` and `@slow`; `ROLE` in `@admin`, `@dangerous` and
`@fast`. `REPLICAOF` changes the running configuration; run `CONFIG REWRITE` to keep the change across restarts
(it writes or removes the `replicaof` line in the file the server started with).

## How a replica syncs

1. **Handshake.** The replica connects (5 s timeout), sends `AUTH` if `masterauth` is set, `PING`, and
   `REPLCONF listening-port <port>`, then `REPLSYNC <replication-id> <offset>` with the primary id and offset it
   stored last time (`? 0` if it has none; a server that was a primary until now sends its own id and its
   latest sequence plus one).
2. **Continue or full sync.** The primary answers `+CONTINUE` if the id is its own and it still keeps the WAL
   from that offset, or if the id is that of the primary it followed before its promotion and the offset is not
   past the promotion point (see [Promotion](#promotion)). Otherwise, or the first time, it
   answers `+FULLSYNC`. A primary without a kept WAL refuses:
   `ERR replication needs the WAL kept on the primary: set wal-retention`.
3. **Full sync.** The replica deletes all its data, index definitions included. The primary reads a RocksDB
   snapshot and sends every column family in batches of about 1 MB, then streams from the snapshot's sequence.
   The replica records the primary's id and offset when the copy is complete. Until then, data commands on the
   replica fail with `LOADING`. A full sync interrupted by a disconnect or restart starts over.
4. **Streaming.** The primary sends each WAL batch as it was committed, with the sequence that follows it, and a
   ping every second when there are no writes. The replica writes each batch and its new offset in a single
   RocksDB write, so its offset always matches its data. Every second the replica sends `REPLCONF ACK <offset>`.
5. **Reconnects.** If the primary sends nothing for 10 s, or the connection fails, the replica reconnects after 1
   s and sends its stored offset, so it continues without a full sync when the primary still keeps that part of
   the WAL. A replica that falls behind the kept WAL while connected is disconnected by the primary and then
   does a full sync.
6. **Restarts.** A replica restarted with `replicaof` in its configuration starts read-only and continues from
   its stored offset (`+CONTINUE`) when possible.

The protocol is datadam's own (`REPLSYNC`, not `PSYNC`): datadam cannot replicate from Redis, and Redis cannot
replicate from datadam. Details: [dev/replication-and-cdc-internals.md](dev/replication-and-cdc-internals.md).

## Behavior on a replica

- **Writes are refused**: every command in `@write` replies `READONLY You can't write against a read only
  replica.` This includes `IDX.CREATE`, `FT.CREATE`, `JSON.SET` and the other `JSON.*` writes. memcache writes
  reply `SERVER_ERROR read only replica`.
- **During a full sync**, every command in `@read` or `@write` replies
  `LOADING datadam is loading the dataset from its primary`. `PING`, `INFO`, `ROLE` and `CONFIG` keep working.
- **Expiry.** A replica hides keys whose TTL has passed, by its own clock, but never deletes them on read: the
  deletion comes from the primary.
- **Indexes.** Index definitions created or dropped on the primary take effect on the replica as soon as the
  batch arrives. The replica never runs backfills; the primary builds the entries and they arrive with the
  stream. `IDX.QUERY` and `FT.SEARCH` therefore see the same index state as the primary, a moment later.
  Repairs that a query would make on a primary are not written on a replica.
- **Cache.** Each applied batch locks the keys it touches and removes them from the object cache, so a
  concurrent read never puts an older value back in the cache. `FLUSHALL` from the primary clears the cache.
- **Change feed.** A replica with its own `wal-retention` writes the replicated batches to its own WAL, so
  `CDC.READ` works there with the replica's own sequence numbers. Records copied by a full sync appear in that
  feed as well; their collection elements carry no type and appear as `data` changes (see
  [change-feed.md](change-feed.md)).

## Monitoring: ROLE and INFO replication

`ROLE` on a replica: `["slave", host, port, state, offset]`, where `state` is `connect` (not connected),
`connecting`, `sync` (full sync under way) or `connected`, and `offset` is the next primary sequence the replica
will apply.

`ROLE` on a primary: `["master", offset, [[ip, port, acked-offset], ...]]`, where `offset` is the primary's
latest sequence plus one.

`INFO replication` on a replica:

| Field | Meaning |
|---|---|
| `role` | `slave` |
| `master_host`, `master_port` | the primary |
| `master_link_status` | `up` when streaming, else `down` |
| `master_last_io_seconds_ago` | seconds since the last message from the primary (-1: never) |
| `master_sync_in_progress` | 1 during a full sync |
| `slave_repl_offset`, `slave_read_repl_offset` | next primary sequence to apply (both the same) |
| `slave_priority` | `replica-priority` |
| `master_link_down_since_seconds` | while the link is down: seconds since it went down |
| `slave_read_only`, `replica_announced` | always 1 |
| `master_replid` | replication id of the primary being followed |
| `full_syncs`, `partial_syncs` | syncs done by this replica since it started |

On every server, primary or replica:

| Field | Meaning |
|---|---|
| `role` | `master` on a primary |
| `connected_slaves` | replicas streaming from this server |
| `slaveN` | `ip=...,port=...,state=...,offset=...,lag=...`: `state` is `sync` (full sync under way) or `online`, `offset` the last acknowledged offset, `lag` the seconds since the last ack |
| `master_replid`, `master_repl_offset` | on a primary: its replication id and latest sequence plus one |
| `master_failover_state` | `no-failover`, `waiting-for-sync` or `failover-in-progress` (see [Failover](#failover)) |
| `own_replid` | this server's replication id |
| `repl_offset` | latest sequence plus one |
| `full_syncs_served`, `partial_syncs_served` | syncs served since start |

The replication lag in writes is the primary's `repl_offset` minus the replica's `slave_repl_offset` (or the
`offset` in its `slaveN` line, which replicas acknowledge as soon as they apply each batch, and at least every
second). `INFO server` also has `run_id`, a random id per process start, which Redis Sentinel uses to notice
restarts.

## Promotion

`REPLICAOF NO ONE` on a replica:

- stops following the primary;
- loads the collection version mark from the replicated data, so new collections never reuse a version the old
  primary used;
- reloads index definitions and resumes any backfill that was still `building`;
- turns writes back on;
- generates a new replication id, and keeps the old primary's id and the offset it had reached.

The other replicas of the old primary, pointed at the promoted one afterwards, continue without a full sync
when they are not past the promotion point: what they miss is in the promoted server's own WAL (each batch it
applied carries the old primary's offset after it), so it streams from the matching place. The usual case after
a failover, since Sentinel promotes the replica that got furthest. So does the old primary, if it took no write
after the promotion point (a [`FAILOVER`](#failover) hand-over, or a primary that stopped). A server that is
past it, with writes the new primary never got, does a full sync and loses them: datadam does not reconcile
histories. That includes writes the old primary acknowledged, even with `WAIT`, after the promotion: they
were never on the new primary.

For a hand-over that loses nothing, use [`FAILOVER`](#failover) on the primary, or set `promotion-handoff yes`
on the replicas: then `REPLICAOF NO ONE` on a replica that is streaming from its primary asks the primary to
hand over, and the primary runs the same steps as `FAILOVER` towards it (see
[Promotion with a hand-over](#promotion-with-a-hand-over)). For automatic failover, Redis Sentinel.

## WAIT

Replication is asynchronous: a write is acknowledged to its client once it is in the primary's WAL. `WAIT`
lets a client make sure its writes reached replicas too:

```text
> SET order:42 paid
OK
> WAIT 1 1000
(integer) 1
```

`WAIT numreplicas timeout` takes the primary's current WAL position (after committing the connection's
pending writes) and waits, without holding a server thread, until `numreplicas` online replicas have
acknowledged it, or `timeout` milliseconds passed (`0`: no limit). It replies how many had. Replicas
acknowledge right after applying each batch, so on a healthy link `WAIT` returns within about a round trip.
Like in Redis, `WAIT` does not make the write durable on the replicas' disks beyond what their `fsync` policy
gives, and a write `WAIT` timed out on is not rolled back. On a replica it replies an error; inside `MULTI` it
answers at once.

## Replication over TLS

With `tls-replication yes`, the replica connects to its primary with TLS:

```text
# on the replica
tls-replication yes
tls-ca-cert-file /etc/datadam/ca.crt          # CAs that sign the primary's certificate
tls-cert-file /etc/datadam/replica.crt        # presented if the primary asks for a client certificate
tls-key-file /etc/datadam/replica.key
replicaof primary.example.com 6380            # the primary's tls-port
```

- **The primary is checked.** Its certificate must be signed by one of the CAs in `tls-ca-cert-file` (the
  system's CAs when that is empty) and name the host given in `replicaof`, as a DNS name or an IP address in its
  subject alternative names. Otherwise the handshake fails, the link stays down, and the replica retries every
  second; the server log says why.
- **The replica can be checked too.** It presents `tls-client-cert-file` (or `tls-cert-file`), so a primary with
  `tls-auth-clients yes` accepts it, and with `tls-auth-clients-user cn` it logs in as the ACL user named by the
  certificate's CN, without `masterauth`.
- The setting is read at each connection: `CONFIG SET tls-replication yes` then `REPLICAOF host tls-port`
  switches a running replica.

## Failover

### FAILOVER: a planned hand-over

`FAILOVER` on the primary makes a replica the primary without losing a write, then makes the old primary its
replica:

1. Writes are refused from then on (`READONLY datadam is handing over to a replica (FAILOVER)`); writes
   already under way finish.
2. The primary waits until the target replica has acknowledged everything (`master_failover_state` is
   `waiting-for-sync`).
3. It tells the replica in the replication stream (`failover-in-progress`); the replica, if it is at exactly
   that position, answers and becomes a primary.
4. The old primary follows it, continuing where it handed over (no full sync), and its `replicaof` setting
   points at it.

`FAILOVER` replies `OK` once the hand-over has started; watch `INFO replication` for its end. In tests on one
machine it takes about 30 ms.

| Form | Meaning |
|---|---|
| `FAILOVER` | To the online replica that is furthest along. |
| `FAILOVER TO host port` | To that replica (as listed in `slaveN`: its address and listening port). |
| `FAILOVER TIMEOUT ms` | Give up (and take writes again) if the replica has not caught up by then. Without it, it waits as long as needed. |
| `FAILOVER TO host port TIMEOUT ms FORCE` | After the timeout, hand over anyway: writes the replica had not received are lost. |
| `FAILOVER ABORT` | Cancel a hand-over that is still waiting for the replica. |

Errors: `ERR FAILOVER requires connected replicas.`, `ERR FAILOVER target HOST and PORT is not a replica.`,
`ERR FAILOVER is not valid when server is a replica.`, `ERR FAILOVER already in progress.`,
`ERR No failover in progress.`. If the replica does not answer within 5 seconds, the primary gives up and takes
writes again. It is in `@admin` and `@dangerous`.

### Promotion with a hand-over

Redis Sentinel promotes a replica with `REPLICAOF NO ONE`, also for a planned `SENTINEL FAILOVER`, while the old
primary is alive. Until Sentinel re-points it, the old primary goes on taking writes and its other replicas
acknowledge them (`WAIT`); none of them reach the promoted replica, so they are lost, and the servers that took
them sync in full. In a test with `WAIT 1` writes from four clients, 14,000 acknowledged writes were lost and
two servers synced in full; in the cloud test, at 10 GB, that took a minute.

With `promotion-handoff yes` on a replica, `REPLICAOF NO ONE` replies `OK` and asks the primary, in the
replication stream (`REPLCONF HANDOFF`), to hand over. The primary does what `FAILOVER TO` that replica does:
it refuses writes, waits until the replica has them all (at most a second), tells it to take over, and follows
it. It does so only for a replica whose user (the one it logs in with, `masteruser`) may run `FAILOVER`;
otherwise it logs the refusal and the replica promotes itself after 5 seconds. The same test lost no write and nothing synced in full: the old primary and the other replica stop at the
promotion point and continue from the new primary. The hand-over takes about 10 ms; meanwhile `ROLE` still says
`slave`, which Sentinel waits out.

The replica becomes a primary on its own, as without the option, when its link to the primary is down (the
primary died or is cut off: there is nothing to hand over), when it is still in a full sync, and when the primary
has not handed over within 5 seconds (it then gives up and takes writes again, as a `FAILOVER` that timed
out). It is off by default because `REPLICAOF NO ONE` also serves to detach a copy, for a test or a
migration: with the option on, the primary would follow that copy. Set it where `REPLICAOF NO ONE` only means a
failover, as with Sentinel; it can be changed with `CONFIG SET`.

### Automatic failover with Redis Sentinel

datadam works with Redis Sentinel (`redis-sentinel` from Redis 7), which watches the primary, agrees among
several sentinels that it is down, promotes the best replica and re-points the others, and tells clients where
the primary is (`SENTINEL get-master-addr-by-name`). Point the sentinels at the primary as you would at Redis:

```text
port 26379
sentinel monitor mymaster 10.0.0.1 6379 2
sentinel down-after-milliseconds mymaster 5000
sentinel failover-timeout mymaster 60000
```

Everything Sentinel needs is there: `INFO` with `run_id`, the replicas' `slaveN` lines and their
`slave_repl_offset`, `slave_priority` and `master_link_down_since_seconds`; `REPLICAOF`, `CONFIG REWRITE` and
`CLIENT KILL TYPE` inside `MULTI`/`EXEC`; and Pub/Sub for the sentinels' `__sentinel__:hello` channel.
`tests/docker_sentinel_test.sh` runs three sentinels against a primary and a replica, stops the primary, and
checks that the replica is promoted, takes writes, and that the old primary comes back as its replica.

Give every server that may become a primary a `wal-retention`, since the others then replicate from it, and
use the same `masteruser`/`masterauth` everywhere. As with Redis, a write acknowledged by a primary that dies
before streaming it is lost; use `WAIT` where that matters. Set `replicaof` in the config file (or with
`REPLICAOF` and `CONFIG REWRITE`), not on the command line: Sentinel re-points a replica by rewriting the
config, and a `--replicaof` would win again at the next restart, so the server would follow a stale primary
until Sentinel corrects it once more.

## Limits

- **Not compatible with Redis replication** (`PSYNC`/`SYNC`): a datadam replica follows a datadam primary.
- **A server that starts following another one does a full sync**, except a replica (or the old primary) of the
  primary a promoted server followed, as long as it is not past the promotion point. Only the last history is
  kept: after two promotions in a row, the replicas of the first primary sync in full.
- **`SENTINEL FAILOVER` does not pause the old primary** unless the replicas have `promotion-handoff yes` (see
  [Promotion with a hand-over](#promotion-with-a-hand-over)). Without it, until Sentinel re-points them, the old
  primary takes writes and its other replicas acknowledge them (`WAIT`), and none of them reach the promoted
  replica: in the cloud test, with writes at 6000/s, about two seconds of acknowledged writes were lost, and the
  replicas past the promotion point synced in full. Redis has the same window.
- **Full syncs** copy the whole database and delete the replica's data first; the primary needs enough
  `wal-retention` to cover one.
- **No chained replication** (a replica of a replica): a replica refuses `REPLSYNC`, and a primary that becomes
  a replica closes its replicas' streams, as Redis does. What a replica would stream is its own WAL, whose
  positions no other server shares: a replica of it went past the promotion point and synced in full when
  Sentinel re-pointed it. Until then it keeps retrying, and serves what it had.

## Runbook

### Add a replica

1. On the primary, make sure `wal-retention` is set (`CDC.INFO` shows `enabled yes`). If not, set it in the
   configuration file and restart the primary.
2. If the primary requires authentication, create a replication user on it:
   `ACL SETUSER replicator on >change-me +ping +replconf +replsync`, then `CONFIG REWRITE`.
3. On the new server, set `masteruser` and `masterauth` (in the file, or with `CONFIG SET`), and
   `wal-retention` if it may become a primary later.
4. Run `REPLICAOF <primary-host> <primary-port>`, then `CONFIG REWRITE` so a restart keeps following.
5. Watch `INFO replication` on the replica until `master_link_status:up` and `master_sync_in_progress:0`, and
   on the primary until the replica's `slaveN` line shows `state=online`.

### Promote a replica

1. If the old primary is still up, stop writes to it (stop clients, or `ACL SETUSER` to remove write
   permissions), and wait until the replica's `slave_repl_offset` equals the primary's `repl_offset`.
2. On the replica: `REPLICAOF NO ONE`, then `CONFIG REWRITE` (otherwise a restart would make it follow the old
   primary again).
3. Point clients at the new primary.
4. If any `IDX.*` or `FT.*` index was still `building`, its backfill resumes on the new primary; check
   `IDX.INFO`.

### Re-point other replicas and the old primary

1. On each other replica: `REPLICAOF <new-primary-host> <new-primary-port>`, then `CONFIG REWRITE`. Each
   continues where it is, unless it got writes the new primary never had: then it does a full sync.
2. To bring the old primary back as a replica, add `replicaof <new-primary-host> <port>` (and `masteruser` /
   `masterauth` if needed) to its configuration file before starting it, so it starts read-only and never takes
   writes as a second primary. If it is already running, run `REPLICAOF <new-primary-host> <port>` and
   `CONFIG REWRITE`. It continues if it took no write after the promotion point; otherwise its data is
   replaced by a full sync.
3. The new primary needs `wal-retention` for this; if it was started without it, set it and restart before
   re-pointing.
