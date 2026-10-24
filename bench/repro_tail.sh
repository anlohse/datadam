#!/usr/bin/env bash
# Reproduces one workload of the "big" scenario against one datadam image and
# prints what the server saw (batch conflicts, cache, RocksDB stalls).
# usage: bench/repro_tail.sh [image] [workload args...]
#   default: datadam:latest, mixed gaussian 20% writes, pipeline 16, 30 s
# env: KEYS, TIME, EXTRA (server arguments), DOCKER_OPTS (docker run options)
set -uo pipefail
export MSYS_NO_PATHCONV=1
IMAGE=${1:-datadam:latest}
shift || true
WORK=("$@")
[ ${#WORK[@]} -eq 0 ] && WORK=(--ratio=1:4 --key-pattern=G:G --pipeline=16)
KEYS=${KEYS:-6000000}
TIME=${TIME:-30}
NET=bench-net SRV=bench-srv VOL=bench-data
BENCH_DIR=$(cd "$(dirname "$0")" && (pwd -W 2>/dev/null || pwd))
docker network create $NET >/dev/null 2>&1
docker rm -f $SRV >/dev/null 2>&1
docker volume rm $VOL >/dev/null 2>&1
docker run -d ${DOCKER_OPTS:-} --name $SRV --network $NET --memory 1g --memory-swap 1g -v $VOL:/var/lib/datadam "$IMAGE" \
  --io-threads 8 --memcache-port 0 --cache-max-memory 384mb --cache-min-memory 64mb --rocksdb-block-cache 128mb \
  --rocksdb-write-buffer 32mb ${EXTRA:-} >/dev/null
for _ in $(seq 100); do
  docker run --rm --network $NET redis:7.4 redis-cli -h $SRV PING 2>/dev/null | grep -q PONG && break
  sleep 0.3
done
docker run --rm --network $NET -v "$BENCH_DIR":/bench:ro python:3.12-slim python /bench/load_random.py $SRV 6379 "$KEYS" 512 4
sleep 10
cli() { docker run --rm --network $NET redis:7.4 redis-cli -h $SRV "$@"; }
before=$(cli INFO persistence | grep -E 'write_batch' | tr -d '\r' | tr '\n' ' ')
cli CONFIG RESETSTAT >/dev/null
cache_counts() { cli INFO cache | tr -d '\r' | grep -E '^cache_(hits|misses|rejected_admission|evicted_lru):' | tr '\n' ' '; }
cache_before=$(cache_counts)
docker run --rm --network $NET redislabs/memtier_benchmark:latest -s $SRV -p 6379 --protocol=redis --threads=4 \
  --clients=25 --data-size=512 --random-data --key-minimum=1 --key-maximum="$KEYS" --hide-histogram \
  --print-percentiles=50,99,99.9,99.99 --test-time="$TIME" "${WORK[@]}" 2>/dev/null | grep -E '^(Type|Sets|Gets|Totals)'
echo "before: $before"
echo "after:  $(cli INFO persistence | grep -E 'write_batch' | tr -d '\r' | tr '\n' ' ')"
echo "cache before: $cache_before"
echo "cache after:  $(cache_counts)"
cli INFO latency | grep usec | tr -d '\r'
cli INFO stats | grep -E 'deferred_reads|read_pool' | tr -d '\r'
cli INFO cache | grep -E 'hit_ratio|evicted_lru|rejected_admission|page_steals|items' | tr -d '\r' | tr '\n' ' '
echo
docker exec $SRV sh -c 'grep -hE "Stalling|stall" /var/lib/datadam/LOG | tail -3; grep -h "Cumulative stall" /var/lib/datadam/LOG | tail -1'
docker rm -f $SRV >/dev/null 2>&1
docker volume rm $VOL >/dev/null 2>&1
