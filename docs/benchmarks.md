# Benchmarks

Measured throughput and latency of datadam against Redis and Kvrocks with memtier_benchmark, with data that
fits in memory and with data three times larger than memory, plus the measured cost of individual features
(TLS, the collection element cache, secondary indexes, JSON, search, the change feed). The raw results are in
`bench/results/`; `bench/RESULTADOS.md` (in Portuguese) is the original report.

- [Summary](#summary)
- [Environment and method](#environment-and-method)
- [Data three times larger than memory](#data-three-times-larger-than-memory)
- [Data that fits in memory](#data-that-fits-in-memory)
- [The tail of the pipelined mixed workload](#the-tail-of-the-pipelined-mixed-workload)
- [Batched reads with MultiGet](#batched-reads-with-multiget)
- [First round](#first-round)
- [Reading the results](#reading-the-results)
- [Limitations](#limitations)
- [Reproducing](#reproducing)
- [Cost of individual features](#cost-of-individual-features)

## Summary

memtier_benchmark with truly random values and independent clients (`--distinct-client-seed`), 100
connections, 512-byte values. Throughput in operations per second, p99.9 latency in milliseconds in
parentheses. "3× memory" means the data set is three times larger than the container's memory.

| Workload | datadam | Kvrocks | Redis |
|---|---|---|---|
| Gaussian reads, data in memory | **476,882** (0.66) | 454,166 (0.80) | 342,820 (1.00) |
| Mixed 20% writes + pipeline 16, in memory | **1,127,578** (13.3) | 265,153 (165) | 593,516 (12.4) |
| Uniform writes, in memory | 198,633 (4.9) | 135,752 (6.4) | **205,887** (10.6) |
| Gaussian reads, 3× memory | **67,669** (4.4) | 35,967 (14.7) | only 22% of the keys fit |
| Mixed 20% writes + pipeline 16, 3× memory | **79,319** (113) | 45,157 (199) | writes refused (OOM) |
| Uniform writes, 3× memory | **134,812** (28) | 45,638 (422) | writes refused (OOM) |

## Environment and method

These results are from the second round, run on 2026-10-22 after the tail-latency investigation described
below. The raw data (memtier JSON and text output, memory and disk usage) is in `bench/results/`:

| Scenario | Independent clients (`distinct`) | Same seeds (`same`) |
|---|---|---|
| `big`, data 3× memory | `big-distinct-20261022-192201/` | `big-same-20261022-193940/` |
| `fit`, data in memory | `fit-distinct-20261022-195827/` | `fit-same-20261022-201743/` (datadam only) |

The first round (`big-20261022-163520/` and `fit-20261022-161636/`) is summarized under
[First round](#first-round).

- **Machine.** Docker Desktop on Windows 11, with a VM of 28 vCPUs and 7.8 GB of RAM, and a virtual disk on an
  SSD. Each server runs alone in a container with a hard memory limit and no swap, on a fresh volume.
  memtier runs in another container on the same machine and shares the CPU with the server.
- **Versions.** memtier_benchmark 2.5.2, Redis 7.4, Kvrocks 2.17.0 and datadam 0.1.0.
- **Configuration.** The memory budgets are equivalent: about half of the container limit goes to caches, and
  the rest to memtables, connections and the process itself.

  | | `fit` (2 GB limit) | `big` (1 GB limit) |
  |---|---|---|
  | datadam | object pool 1 GB + block cache 256 MB, 8 I/O threads | pool 384 MB + block cache 128 MB, write buffer 32 MB, 8 threads |
  | Kvrocks | block cache 1280 MB, 8 workers (default) | block cache 512 MB, write buffer 32 MB, 8 workers |
  | Redis | maxmemory 1800 MB, noeviction, 4 I/O threads | maxmemory 900 MB, noeviction, 4 I/O threads |

  datadam runs with the default read pool of 64 threads. All three log every write and sync once per second:
  the WAL of datadam and Kvrocks, and Redis's AOF with `everysec`. Redis uses `noeviction` because a database
  must not drop data.
- **Data.** 512 bytes per key, with **truly random** values. memtier's `--random-data` reuses the same random
  block for every key: a thousand of its values compress 89× with gzip. In a store that compresses (RocksDB
  with LZ4), a "3 GB" data set like that takes a few hundred MB and fits in memory. Therefore:
  - the data is loaded by `bench/load_random.py`, with `os.urandom` per key and memtier's key names
    (`memtier-1` … `memtier-N`);
  - memtier then runs the workloads on top of that data;
  - since memtier's own writes are compressible again, every workload that writes starts from a freshly loaded
    data set.
- **Client seeds.** By default memtier gives every connection the same random seed. The 100 connections then
  ask for the same sequence of keys at almost the same time: the first one fetches a key from disk and the
  other 99 find it in the cache. That turns data three times larger than memory into a warm-cache test.
  `--distinct-client-seed` gives each connection its own sequence, which is the realistic case. The main
  round uses `distinct`; `same` is included because it is what memtier does unless told otherwise, and because
  it shows the behavior when many clients want one key at the same moment.
- **Workloads.** 60 s each, with 100 connections (4 threads × 25 clients) and no pipelining, except where
  noted.
  - `read-uniform`: 100% `GET`, uniform keys.
  - `read-gaussian`: 100% `GET` with a Gaussian distribution over the keys.
  - `mixed-gaussian`: 20% `SET` and 80% `GET`, Gaussian.
  - `mixed-gaussian-p16`: the same, with a pipeline of 16.
  - `write-uniform`: 100% `SET`, uniform keys, overwriting.
- **Scenarios.**
  - `fit`: 1 million keys (about 0.5 GB of values) in 2 GB containers. Everything fits in memory.
  - `big`: 6 million keys (about 3 GB of values) in 1 GB containers. The data is **3× larger than memory**,
    and the page cache also counts against the limit.
- **Compare only within one round.** The container's disk is a file on Windows, which the host caches in its
  own RAM when memory is free, so the speed of the "disk" changes with the rest of the machine. The same
  Kvrocks made 212,000 `GET`/s in the `big` scenario of the first round and 73,000/s in this one, with the same
  memtier settings. The servers are therefore compared with each other within each round, and before/after
  measurements of datadam alternated the versions run by run.
- **memtier's maximum latency.** memtier 2.5.2 records, in almost every run and for every server, one sample
  of about 604 s (`Max Latency: 603979.775`), which inflates the average latency. The tables use percentiles
  only.

## Data three times larger than memory

Independent clients (`distinct`, the realistic case). Each cell is ops/s · latency p50 / p99 / p99.9 in ms:

| Workload | datadam | Kvrocks |
|---|---|---|
| read-uniform | **54,056** · 1.61 / 6.62 / 9.60 | 33,545 · 2.80 / 10.75 / 16.51 |
| read-gaussian | **67,669** · 1.48 / 3.10 / 4.45 | 35,967 · 2.65 / 9.66 / 14.65 |
| mixed-gaussian | **62,467** · 1.35 / 6.11 / 9.60 | 38,382 · 2.16 / 12.61 / 24.83 |
| mixed-gaussian-p16 | **79,319** · 15.81 / 76.80 / 112.64 | 45,157 · 33.28 / 134.14 / 198.66 |
| write-uniform | **134,812** · 0.43 / 6.14 / 28.03 | 45,638 · 0.39 / 1.61 / 421.89 |

Same seeds (`same`, memtier's default):

| Workload | datadam | Kvrocks |
|---|---|---|
| read-uniform | 62,177 · 1.41 / 6.30 / 12.03 | **72,751** · 1.33 / 5.31 / 9.66 |
| read-gaussian | 84,429 · 1.34 / 2.53 / 3.74 | **88,530** · 1.30 / 3.18 / 3.77 |
| mixed-gaussian | 66,640 · 1.30 / 5.98 / 12.03 | **93,016** · 1.26 / 3.41 / 4.80 |
| mixed-gaussian-p16 | **408,342** · 3.26 / 13.76 / 43.26 | 113,918 · 13.44 / 30.46 / 67.07 |
| write-uniform | 199,026 · 0.38 / 4.93 / 8.32 | **208,536** · 0.29 / 0.79 / 2.70 |

Resources, `distinct` round:

| | datadam | Kvrocks |
|---|---|---|
| Loading 6 M keys | 27.5 s | 49.6 s |
| Memory after loading | 549 MiB | 159 MiB |
| Disk after loading | 5,760 MB | 6,356 MB |
| Disk at the end | 10,878 MB | 12,053 MB |
| Process killed for lack of memory | no | no |

Redis was not run again in `big`: with `noeviction` it holds only the first 1.33 million keys (22%) and
refuses the writes after that, as the first round showed.

## Data that fits in memory

Independent clients (`distinct`):

| Workload | datadam | Kvrocks | Redis |
|---|---|---|---|
| read-uniform | **490,755** · 0.17 / 0.49 / 0.65 | 475,544 · 0.19 / 0.57 / 0.79 | 341,654 · 0.29 / 0.66 / 1.08 |
| read-gaussian | **476,882** · 0.18 / 0.51 / 0.66 | 454,166 · 0.20 / 0.60 / 0.80 | 342,820 · 0.30 / 0.62 / 1.00 |
| mixed-gaussian | **425,127** · 0.20 / 0.57 / 0.81 | 282,481 · 0.21 / 2.16 / 18.30 | 278,182 · 0.36 / 0.78 / 1.41 |
| mixed-gaussian-p16 | **1,127,578** · 1.14 / 10.75 / 13.25 | 265,153 · 3.57 / 58.37 / 164.86 | 593,516 · 2.37 / 7.42 / 12.35 |
| write-uniform | 198,633 · 0.36 / 3.60 / 4.86 | 135,752 · 0.61 / 2.99 / 6.43 | **205,887** · 0.46 / 1.23 / 10.62 |

| `distinct` | datadam | Kvrocks | Redis |
|---|---|---|---|
| Loading 1 M keys | 3.8 s | 5.1 s | 3.2 s |
| Memory after loading | 926 MiB | 94 MiB | 688 MiB |
| Disk after loading | 551 MB | 1,077 MB | 508 MB |

With the same seeds (`same`), datadam made 470,512 / 457,185 / 426,885 / **1,596,645** / 281,706 ops/s in the
five workloads (p99.9 of 0.75 / 0.76 / 0.82 / 3.36 / 1.41 ms). In the first round, before the changes that came
out of the investigation below, the figures were 486,400 / 481,709 / 443,663 / 1,694,585 / 298,455. The 3 to
6% difference is within the variation of this environment, so those changes did not slow down the case where
everything fits in memory.

## The tail of the pipelined mixed workload

In the first round, `mixed-gaussian-p16` in the `big` scenario gave datadam a p99.9 of 102 ms, against 37 ms
for Kvrocks. Measuring inside the server, with latency histograms (the ones `INFO latency` reports), showed
where the time went and found three chained causes.

1. **Batch locks held during disk reads.** A pipeline held the locks of the keys it wrote until the end of
   the pipeline. `GET`s that missed the cache locked their keys too, and read the disk while holding all of
   that. Another client that needed one of those keys waited, blocking one of the 8 I/O threads. Measured:
   100,000 waits in 30 s, with a median of 1.3 ms and a p99.9 of 24 ms, while the RocksDB commits themselves
   took 6 µs. **Fix:** reads do not join the batch (they lock the key only for the read itself), and the
   batch is committed before any disk read. The time a batch holds locks dropped from 7 ms to 0.2 ms at the
   p99.9, and the conflicts from 61,000 to 800.
2. **The same key wanted by everyone at once.** With memtier's same seeds, 99.98% of the remaining waits were
   for *the same key* another client was reading from disk. Each wait blocked an I/O thread, and with 8
   threads the whole server stopped on every disk read. The cache admission filter made it worse, because it
   refused the key on its first access and forced a second disk read. The same thing happens for real when a
   hot key leaves the cache (a cache stampede).
3. **Only 8 disk reads at a time.** With independent clients nearly every `GET` goes to disk, and each read
   took an I/O thread. With 8 threads the server made 16,000 to 25,000 ops/s; with 32 I/O threads the same
   test reached 76,000, which confirmed the cause.

The fix for 2 and 3 is the **read pool** (see
[Operations](operations.md#disk-reads-and-the-read-pool)): a plain read that misses the cache leaves the I/O
thread and waits asynchronously; a pool of threads reads the key once for everyone who wants it, and the keys
of the rest of the pipeline are loaded in parallel.

Before and after, `mixed-gaussian-p16` in `big`, 30 s per run, alternating the versions (two runs each):

| Seeds | Version | ops/s | p99 (ms) | p99.9 (ms) |
|---|---|---|---|---|
| same | before | 267,926 / 245,888 | 23.9 / 35.6 | 38.1 / 59.6 |
| same | locks fixed; a blocked read retried by a timer (no pool) | 304,980 / 302,163 | 22.3 / 24.4 | 32.5 / 45.8 |
| same | **with the read pool** | **575,507 / 569,065** | **6.7 / 6.6** | **17.8 / 19.3** |
| distinct | before | 25,300 / 16,073 | 309 / 332 | 403 / 377 |
| distinct | **with the read pool** (32 threads) | **66,251 / 72,351** | **101 / 97** | **148 / 126** |
| distinct | read pool with 64 / 128 threads | 81,885 / 88,766 | — | 84 / 75 |

Without pipelining, with independent clients and `read-uniform`, the read pool made 32,793 ops/s (p99.9 of
22.5 ms), against 8,432 ops/s (p99.9 of 97 ms) for the same binary with `read-threads 0`. With the same seeds
and no pipelining, the versions were within the noise (before: 64,000 and 43,000; with the pool: 61,000 and
51,000; without the pool: 63,000 and 69,000). In that case every client follows a single one, and going
through the pool adds a few microseconds to its path.

A cache **admission filter** (TinyLFU) was also tested, on the hypothesis that one-off reads pushed the hot
keys out. In this workload it did not change the cache hit ratio, 96% with or without it. The filter stays,
because it keeps a bulk load from replacing the whole cache, but it was not the cause of the tail.

## Batched reads with MultiGet

The `read-batch` option lets each read thread take up to `read-batch` keys when more keys are queued than
there are threads, and read them all with one RocksDB `MultiGet`. Measured with `mixed-gaussian-p16` in `big`,
independent clients, 30 s per run, alternating the settings in the same time window:

| Setting | ops/s in each run | Mean | p99.9 (median) |
|---|---|---|---|
| `read-batch 1` (one read per key) | 39,425 / 71,282 / 51,680 / 89,149 / 90,785 | 68,464 | 110 ms |
| `read-batch 16` | 92,903 / 79,377 / 53,753 / 63,972 / 68,590 | 71,719 | 110 ms |
| `read-batch 16` with io_uring (`seccomp=unconfined`) | 72,229 / 51,616 | 61,923 | 128 ms |

The difference is within the noise. The histograms show why: an average batch of 2.5 to 2.8 keys took 3 to
3.5 ms at the p50, while a single `Get` took 0.45 ms. RocksDB reads in parallel, with io_uring, only the blocks
of one SST file; different files are read one after another unless RocksDB is built with folly's coroutines.
Random keys over 3.6 GB almost never fall in the same file, so a batch trades parallel reads (one per thread)
for sequential ones and saves only CPU. The code is kept, covered by tests, for workloads with nearby keys;
the default is `read-batch 1`, and the build does not link liburing. Reading the disk truly in parallel would
need asynchronous reads across files.

## First round

Run the same day, with memtier's same seeds, before the investigation above. The "disk" was much faster at
that time (see the method), so the absolute numbers cannot be compared with the ones above.

| `big` (ops/s · p50 / p99 / p99.9 ms) | datadam | Kvrocks | Redis |
|---|---|---|---|
| read-uniform | 233,541 · 0.38 / 2.00 / 3.44 | 212,432 · 0.48 / 1.52 / 2.17 | only 22% of the keys exist |
| read-gaussian | 264,505 · 0.38 / 1.43 / 2.13 | 224,812 · 0.45 / 1.66 / 2.22 | only 22% of the keys exist |
| mixed-gaussian | 214,263 · 0.38 / 2.19 / 4.38 | 230,515 · 0.37 / 1.98 / 3.07 | 100% of the `SET`s refused |
| mixed-gaussian-p16 | 250,040 · 3.92 / 28.93 / 102.40 | 324,911 · 3.58 / 21.12 / 37.38 | 100% of the `SET`s refused |
| write-uniform | 285,367 · 0.34 / 0.68 / 3.85 | 262,971 · 0.30 / 0.78 / 2.06 | 100% of the `SET`s refused |

In the `fit` scenario of that round, datadam led all five workloads: 1.69 M ops/s with pipelining, against
1.10 M for Redis and 0.87 M for Kvrocks. Redis kept only 1.33 M of the 6 M keys in `big` and refused 4.67 M.

## Reading the results

1. **Data larger than memory, independent clients: datadam leads every workload.** It makes 1.6× to 1.9×
   Kvrocks's throughput in the reads and the mixed workload, and 3× in pure writes, with a lower p99.9 in all
   of them. In the pipelined mixed workload the p99.9 is 113 ms against 199 ms for Kvrocks, with 1.8× the
   throughput.
2. **With the same seeds, Kvrocks wins the workloads without pipelining.** The difference is 5% in Gaussian
   reads and pure writes, 17% in uniform reads and 40% in the mixed workload. In that test memtier makes the
   100 clients follow a single one, which goes to disk, and each of that client's reads goes through the read
   pool. In the mixed workload the 100 clients also write the same key at almost the same time and contend for
   its lock. With pipelining, datadam makes 3.6× Kvrocks's throughput.
3. **With the data in memory, datadam leads everything except pure writes**, where Redis makes 4% more. With
   pipelining datadam makes 1.13 M ops/s, against 594,000 for Redis and 265,000 for Kvrocks. The price is
   memory: datadam uses the pool it is given (926 MiB, against 688 MiB for Redis).
4. **Redis does not work as a database when the data is larger than memory.** Configured not to drop data
   (`noeviction`), it refuses writes.
5. **Disk.** At the end of the `big` scenario with independent clients, datadam took 10.9 GB and Kvrocks
   12.1 GB, for 3.1 GB of values. Writes spread over 6 million keys leave RocksDB's compaction behind in both.
   Kvrocks also keeps its WAL for 3 hours by default (`rocksdb.wal_ttl_seconds 10800`).
6. **What limits datadam in `big` is the disk itself.** The p50 of a RocksDB read was about 450 µs, and in the
   pipelined workload the read pool's queue reaches 7 ms. Reading keys in batches with `MultiGet` did not help
   (see [Batched reads with MultiGet](#batched-reads-with-multiget)).

## Limitations

- memtier, the loader and the server share the same 28 vCPUs. On a real network, the network latency would be
  added and the client's CPU would not compete with the server's.
- The disk is Docker Desktop's virtual disk on a Windows SSD, with the host's cache and antivirus in between,
  and its speed changes with the host's free memory. On a dedicated NVMe drive, the workloads that read from
  disk should improve for both datadam and Kvrocks.
- Each workload runs 60 s, once per round. Repeated runs of the same workload varied by 10% to 40% in this
  environment; smaller differences are not conclusive.
- memtier 2.5.2's bogus maximum latency (see the method) makes its average latency meaningless here.
- Strings only (`GET`/`SET`). Collections and secondary indexes have their own measurements below.

## Reproducing

Everything runs in Docker. Build the image first; the scripts use `datadam:latest` and pull
`redislabs/memtier_benchmark:latest`, `redis:7.4`, `apache/kvrocks:latest` and `python:3.12-slim`.

```bash
docker build -t datadam .
SEEDS=distinct bash bench/memtier.sh big          # or fit; SEEDS=same reproduces memtier's default
bash bench/memtier.sh fit datadam redis          # only some of the servers
python bench/report.py bench/results/<scenario>-<seeds>-<date>
```

`bench/memtier.sh <fit|big> [server ...]` runs the servers one at a time (`datadam`, `redis`, `kvrocks` by
default), each in a fresh container and volume with the memory limit of the scenario. For each server it loads
the data with `bench/load_random.py`, records memory and disk use, runs the five workloads, and reloads the data
before every workload that writes. Results go to `bench/results/<scenario>-<seeds>-<time>/`: memtier's JSON
and text output per workload, and `notes.txt` with load times, memory, disk and whether the container was
killed for lack of memory. `bench/report.py` turns a results directory into tables.

| Variable | Default | Meaning |
|---|---|---|
| `SEEDS` | `distinct` | `distinct` passes `--distinct-client-seed`; `same` uses memtier's default |
| `KEYS` | `1000000` (`fit`), `6000000` (`big`) | Number of keys |
| `DATA` | `512` | Value size in bytes |
| `MEM` | `2g` (`fit`), `1g` (`big`) | Container memory limit |
| `TIME` | `60` | Seconds per workload |

The datadam arguments per scenario are in `server_cmd` inside the script (`--io-threads 8 --memcache-port 0`
plus the cache sizes of the configuration table above).

`bench/repro_tail.sh [image] [memtier workload args...]` reproduces one workload of the `big` scenario against
one datadam image and prints what the server saw: the group-commit counters before and after, the cache
counters, `INFO latency` (cleared with `CONFIG RESETSTAT` before the run), the read pool counters and RocksDB's
stall lines from its `LOG`. By default it runs the pipelined mixed workload (`--ratio=1:4 --key-pattern=G:G
--pipeline=16`) for 30 s with memtier's same seeds; workload arguments replace that default.

| Variable | Default | Meaning |
|---|---|---|
| `KEYS` | `6000000` | Number of keys loaded |
| `TIME` | `30` | Seconds of the run |
| `EXTRA` | empty | Extra datadam arguments, for example `--read-batch 16` |
| `DOCKER_OPTS` | empty | Extra `docker run` options, for example `--security-opt seccomp=unconfined` |

```bash
bash bench/repro_tail.sh datadam:latest --ratio=1:4 --key-pattern=G:G --pipeline=16 --distinct-client-seed
EXTRA="--read-threads 128" bash bench/repro_tail.sh
```

## Cost of individual features

These measurements come from the feature descriptions in the project README. Unless stated otherwise they
were taken in Docker with `redis-benchmark`, in thousands of requests per second.

### TLS

redis-benchmark, 50 clients:

| | No pipeline | Pipeline 16 |
|---|---|---|
| `SET` / `GET` without TLS | 150 / 154 | 995 / 2,061 |
| `SET` / `GET` with TLS | 111 / 109 | 719 / 1,273 |

### ACL checks

For a user with full access, the common case, the check costs one atomic read per command. For a restricted
user, it is a few bits and one glob match per key. In a redis-benchmark run with a pipeline of 16 there was no
measurable difference.

### Key locks

Key locks spin briefly (`spin_try_lock`) before sleeping on the mutex, because a lock is held for
microseconds and waking a thread through the kernel costs more than that. With 50 clients running `INCR` on a
single counter:

| | No pipeline | Pipeline 16 |
|---|---|---|
| Locks that sleep at once | 92 | 718 |
| Spin first (current behavior) | 150 | 2,000 |
| Redis with AOF, same test | 159 | 1,400 |

### Collection element cache

Small collections (up to 128 elements that fit in `cache-max-object-size`) are kept whole in the object
cache. redis-benchmark, 50 clients, thousands of requests per second without pipeline / with pipeline 16; a
build without the element cache and the current build, measured in the same round:

| Test | Without element cache | With element cache |
|---|---|---|
| `ZRANGE z 0 -1` (sorted set of 20 members, scores changing) | 1 / 2 | 130 / 775 |
| `HGETALL h` (hash of 20 fields being rewritten) | 56 / 71 | 118 / 523 |
| `LRANGE l 0 -1` (10 elements) | 144 / 324 | 140 / 1,136 |
| Hot `HGET` / `SISMEMBER` / `LINDEX` | ~152 / 1,400–1,700 | ~152 / 2,000–2,150 |
| `SADD` into a set of 20 members | 151 / 1,626 | 152 / 2,127 |
| `HGETALL` over 100,000 small hashes | 148 / 651 | 146 / 1,369 |
| `HSET` over 100,000 small hashes | 156 / 458 | 157 / 560 |
| `HSET` into a hash of 20 fields | 152 / 615 | 153 / 584 |
| `ZADD` into a sorted set of 20 members (writes only) | 160 / 273 | 141 / 221 |

The pathological case is reading a small collection that changes a lot: without the element cache, the
RocksDB iterator has to skip every old version and every tombstone of the same keys. The price of the cache
shows in pure `ZADD` on a hot sorted set: each `ZADD` changes three elements (the member and the score index),
and the in-memory set is published to the cache on every commit.

### Lua scripts

redis-benchmark, 50 clients, keys drawn from 100,000, thousands of requests per second; Redis 7.4 without
persistence, datadam with its defaults (every write in the WAL, `fsync everysec`), in the same run:

| Test | datadam, no pipeline | Redis, no pipeline | datadam, pipeline 16 | Redis, pipeline 16 |
|---|---|---|---|---|
| `INCR` (no script, for reference) | 148 | 156 | 748 | 1,408 |
| `EVAL "return 1" 0` | 152 | 157 | 984 | 909 |
| `EVAL` with one `GET` | 151 | 165 | 545 | 585 |
| `EVAL` with one `INCR` | 156 | 170 | 359 | 587 |
| `EVALSHA` with one `INCR` | 154 | 163 | 439 | 619 |
| `EVAL` with `GET`, `SET` and `INCR` on two keys | 159 | 142 | 217 | 361 |

Without a pipeline the network sets the pace for both. With one, scripts run one at a time, as in Redis:
what runs alone is the Lua code and its commands; the RocksDB write of a script's changes happens after it
gives the keyspace back, overlapping the next scripts. Two changes made this possible: taking the keyspace
before the engine's mutex (scripts waiting for their turn spin instead of sleeping, which took `EVAL
"return 1"` from about 100k to 350k requests per second on the development machine), and committing without
the keyspace. Together they took `EVAL` with one `INCR` from 54k to 156k requests per second without a
pipeline, and from 62k to 359k with one.

### Secondary indexes on hashes

Docker, 100,000 hashes, 50 clients:

| Operation | Thousands/s |
|---|---|
| `HSET` through `redis-cli --pipe`, no index / 1 index / + 1 `UNIQUE` index | 224 / 170 / 126 |
| `IDX.GET` on a `UNIQUE` index, no pipeline / pipeline 16 | 139 / 641 |
| `IDX.QUERY city = c42 age < 30 LIMIT 0 10`, no pipeline / pipeline 16 | 92 / 158 |
| `IDX.QUERY city = c42 LIMIT 0 100 NOCONTENT` | 95 |
| `IDX.COUNT` over 1,000 hashes | 41 (reading only the index; checking each hash measured 1.8) |

`UNIQUE` costs more because each write to it is committed at once instead of waiting for the rest of the
pipeline. See [Secondary indexes](secondary-indexes.md).

### JSON

With a document of about 40 bytes and 100 connections:

| Operation | Thousands/s |
|---|---|
| `JSON.SET` | 292 |
| `SET` with the same text | 312 |
| `JSON.GET` of the whole document | 470 (the same as `GET`) |
| `JSON.GET $.age` | 451 |
| `JSON.NUMINCRBY` | 304 |

Large documents cost in proportion to their size on every write, since each write reads, changes and stores
the whole document. See [JSON](json.md).

Secondary indexes on JSON documents (document of about 40 bytes, 100,000 keys, 100 connections):

| Operation | Thousands/s |
|---|---|
| `JSON.SET`, no index | 315 |
| `JSON.SET` with a two-field index | 289 |
| `JSON.NUMINCRBY` on an indexed field | 213 |
| `IDX.QUERY city = SP LIMIT 0 10`, returning the documents | 106 |

### Search (`FT.*`)

Docker, redis-benchmark with 50 connections; the queries run over 100,000 hashes with 100 colors, 1,000 prices
and 10 sizes:

| Operation | Thousands/s |
|---|---|
| `HSET` of 3 fields with new values: no index / `FT` index of 3 fields / composite `IDX` index of 2 | 222 / 111 / 100 |
| `FT.SEARCH "@color:{c42}" LIMIT 0 10` (1,000 results, returns 10 hashes) | 33 |
| The same with `NOCONTENT` | 40 |
| `FT.SEARCH "@color:{c42} @price:[0 99]"` (intersection of 1,000 with 10,000) | 3.9 |
| `FT.SEARCH "@price:[0 99]" SORTBY price LIMIT 0 10` (10,000 results) | 3.2 |
| `FT.SEARCH "*" LIMIT 0 0` (counts 100,000) | 0.9 |
| `IDX.QUERY color = c42 price < 100 LIMIT 0 10` on the composite index | 80 |

`FT.SEARCH` always computes the total, so it pays for every entry of each field in the query. For a
combination of fields that is queried all the time, a composite `IDX.*` index over the same data reads only
the slice of the page and is 20× faster. Both kinds of index can cover the same prefix. See
[Search](search.md).

### Change feed

With pure pipelined writes (1 million keys, 100 connections), the feed turned on made 568,000 and 570,000
writes per second, against 607,000 to 625,000 with it off: about 7%. The first run with it on made 401,000;
the likely reason is Docker's virtual disk growing to hold the first 1 GB of kept WAL. See
[Change feed](change-feed.md).
