# Client libraries

datadam runs client libraries' own test suites against itself: what an application using that library would
meet, beyond datadam's tests. The scripts are in `tests/clients/`; the [Clients workflow](../.github/workflows/clients.yml)
runs them every night against the image of `main` (`ghcr.io/anlohse/datadam:edge`), on demand against any
image, on a pull request labelled `clients`, and before every release.

| Library | Version | Suite | Result |
|---|---|---|---|
| [redis-py](https://github.com/redis/redis-py) | 8.1.0 | sync and asyncio, standalone, RESP2 and RESP3 | Everything passes but the tests of features datadam does not have, listed in [`tests/clients/redis-py.skip`](../tests/clients/redis-py.skip) with the reason |
| [node-redlock](https://github.com/mike-marcacci/node-redlock) (`redlock` on npm) | 5.0.0-beta.2 | one server, and three independent servers (the Redlock algorithm) | All 12 tests pass; its cluster runs are left out |

Not run: tests that need a cluster, Redis modules (RediSearch's and RedisJSON's own clients), TLS
certificates of their own, or Sentinel (datadam's own Sentinel suite covers it: `tests/docker_sentinel_test.sh`).

## What the skip list says

[`tests/clients/redis-py.skip`](../tests/clients/redis-py.skip) is also a map of what datadam does not do yet
that a Redis client may use: Streams, GEO, `SORT`, sorted-set operations across keys (`ZUNION`, `ZINTER`,
`ZDIFF`...), bitmaps, HyperLogLog, `DUMP`/`RESTORE`, `COPY`, `CLIENT PAUSE`/`TRACKING`, ACL selectors, more
than one database, and Lua's `cmsgpack`. Each group names its reason; when a feature lands, its group is
removed and the suite runs those tests again.

## What the suites found

Running them first found differences from Redis in commands datadam has, now fixed:

- `INFO` had no `arch_bits` nor `cluster_enabled` (redis-py's test setup reads them), and used only its first
  section argument.
- `AUTH` and `HELLO` were checked against the user's command permissions, so a user without them could not log
  in as someone else.
- `COMMAND` returned nothing and `COMMAND INFO` a nil per command; clients that find keys with it (redis-py's
  command parser, cluster clients) could not. It now describes every command, with `COMMAND GETKEYS`.
- A subcommand datadam does not have was refused as a permission error and logged in `ACL LOG`.
- ACL rules naming a Redis command datadam does not have (`+cluster`) were refused.
- `PUBSUB NUMSUB` replied a map in RESP3, where Redis replies a flat array.
- `CONFIG RESETSTAT` did not reset `total_commands_processed`, `commandstats` or `errorstats`.
- `CLIENT LIST` and `CLIENT INFO` had no `laddr`; `CLIENT KILL LADDR` was refused.

## Running them

Docker only; each script starts its servers and a runner on a network of its own, and removes them.

```bash
bash tests/clients/redis-py.sh ghcr.io/anlohse/datadam:edge   # PROTOCOL=2|3, PYTEST_ARGS=tests/test_commands.py
bash tests/clients/redlock.sh datadam:latest                   # an image built here (docker build -t datadam .)
```

Behind a proxy or an antivirus that intercepts HTTPS, `EXTRA_CA=<root CA file>` makes pip and npm trust it.
