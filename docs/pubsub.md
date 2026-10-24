# Pub/Sub

datadam implements Redis Pub/Sub: channels, glob patterns and shard channels, with the same commands, reply
shapes and RESP2/RESP3 behavior. Messages are delivered as soon as they are published, to the subscribers
connected to the same server.

## Contents

- [Commands](#commands)
- [Messages](#messages)
- [Subscribed connections](#subscribed-connections)
- [Delivery and ordering](#delivery-and-ordering)
- [Permissions](#permissions)
- [Limits and differences from Redis](#limits-and-differences-from-redis)

## Commands

| Command | Syntax | Reply |
|---|---|---|
| SUBSCRIBE | `SUBSCRIBE channel [channel ...]` | `["subscribe", channel, count]` per channel |
| PSUBSCRIBE | `PSUBSCRIBE pattern [pattern ...]` | `["psubscribe", pattern, count]` per pattern |
| SSUBSCRIBE | `SSUBSCRIBE shardchannel [shardchannel ...]` | `["ssubscribe", channel, count]` per channel |
| UNSUBSCRIBE | `UNSUBSCRIBE [channel ...]` | `["unsubscribe", channel, count]` per channel; all of them without arguments, or one with a nil channel if there were none |
| PUNSUBSCRIBE | `PUNSUBSCRIBE [pattern ...]` | Same, for patterns |
| SUNSUBSCRIBE | `SUNSUBSCRIBE [shardchannel ...]` | Same, for shard channels |
| PUBLISH | `PUBLISH channel message` | How many subscribers received it |
| SPUBLISH | `SPUBLISH shardchannel message` | How many shard subscribers received it |
| PUBSUB CHANNELS | `PUBSUB CHANNELS [pattern]` | Channels with at least one subscriber |
| PUBSUB NUMSUB | `PUBSUB NUMSUB [channel ...]` | `channel, count` pairs (a map in RESP3) |
| PUBSUB NUMPAT | `PUBSUB NUMPAT` | Patterns with at least one subscriber |
| PUBSUB SHARDCHANNELS | `PUBSUB SHARDCHANNELS [pattern]` | Shard channels with subscribers |
| PUBSUB SHARDNUMSUB | `PUBSUB SHARDNUMSUB [shardchannel ...]` | `channel, count` pairs |

`count` in a confirmation is how many channels and patterns the connection is now subscribed to; for shard
channels it counts shard channels only, as in Redis.

## Messages

```text
$ redis-cli SUBSCRIBE news
1) "subscribe"
2) "news"
3) (integer) 1
1) "message"
2) "news"
3) "hello"
```

| Subscribed with | Message |
|---|---|
| `SUBSCRIBE channel` | `["message", channel, payload]` |
| `PSUBSCRIBE pattern` | `["pmessage", pattern, channel, payload]` |
| `SSUBSCRIBE channel` | `["smessage", channel, payload]` |

A connection subscribed to a channel and to a pattern that matches it receives the message twice, once per
subscription, and `PUBLISH` counts both. Patterns use the same glob syntax as `KEYS` (`*`, `?`, `[...]`, `\`).
`PUBLISH` reaches channel and pattern subscribers; `SPUBLISH` reaches shard channel subscribers only.

In RESP3 (`HELLO 3`), confirmations and messages are pushes (`>`), which clients tell apart from replies.

## Subscribed connections

- **RESP2.** While it has subscriptions, a RESP2 connection may only run `SUBSCRIBE`, `PSUBSCRIBE`,
  `SSUBSCRIBE`, the unsubscribe commands, `PING` and `QUIT`. Others reply `ERR Can't execute 'get': only
  (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context`. `PING` replies
  `["pong", message]` (an empty message without an argument). Once every subscription is gone, the connection
  is a normal one again.
- **RESP3.** Any command may run; replies and pushes are interleaved on the connection.
- **Leaving.** Closing the connection removes its subscriptions.

## Delivery and ordering

- A published message is handed to each subscriber's connection right away; the connection sends it while it
  waits for requests, without needing one.
- Messages from one publisher arrive in the order they were published. Messages from different publishers
  arrive in the order the server handled the `PUBLISH` commands.
- A subscription takes effect when its confirmation is sent: the confirmation always comes before the first
  message on that channel.
- There is no storage: a message published while nobody is subscribed is lost, as in Redis. For durable
  events, use the [change feed](change-feed.md) or a list consumed with `BLPOP`/`BLMOVE`.

## Permissions

ACL users have channel rules, as in Redis 7:

| Rule | Meaning |
|---|---|
| `&pattern` | Channels the user may publish to and subscribe to (glob). |
| `allchannels` | Same as `&*`. |
| `resetchannels` | No channels. New users start with no channels; the `default` user has all. |

`PUBLISH`, `SPUBLISH`, `SUBSCRIBE` and `SSUBSCRIBE` need the channel to match one of the patterns.
`PSUBSCRIBE` needs its pattern to be one of the user's patterns, written the same way (or the user to have
`allchannels`): `&news.*` allows `PSUBSCRIBE news.*` but not `PSUBSCRIBE n*`. A refusal replies
`NOPERM No permissions to access a channel` and is recorded in `ACL LOG` with reason `channel`. The commands
are in the `@pubsub` category. See [Security](security.md).

## Limits and differences from Redis

- **Slow subscribers.** Output waiting for a subscriber is kept up to 32 MB. A subscriber that falls further
  behind is disconnected (like Redis's default `client-output-buffer-limit pubsub 32mb`).
- **Replication.** Messages stay on the server that received the `PUBLISH`; they are not sent to replicas, and
  replicas accept `PUBLISH` and subscriptions locally.
- **No keyspace notifications** (`notify-keyspace-events`).
- **Shard channels** behave like a separate set of plain channels: there is no cluster.
- **Not on memcache.** Pub/Sub is only on the Redis protocol.

Measured cost (Docker, redis-benchmark with 50 connections, one subscriber reading everything): `PUBLISH` at
333 thousand/s without pipelining and 2.66 million/s with a pipeline of 16, while plain `GET` runs at 363
thousand/s in the same setup.

The implementation (the subscriber's output queue and the connection's writer) is described in
[Architecture](dev/architecture.md#pubsub-connections).
