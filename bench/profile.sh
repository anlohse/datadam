#!/usr/bin/env bash
# Profiles datadam with perf while memtier runs one workload, in Docker: the
# server's container plus a privileged sidecar (perf) in its PID namespace.
# usage: bench/profile.sh [image] [memtier workload args...]
#   default: datadam:latest, uniform writes, 100 connections, 30 s
# env: KEYS (1000000), TIME (30), EXTRA (server arguments)
# The perf tools image: docker build -t perf-tools - with
#   FROM ubuntu:24.04 + apt-get install linux-tools-generic
set -uo pipefail
export MSYS_NO_PATHCONV=1
IMAGE=${1:-datadam:latest}
shift || true
WORK=("$@")
[ ${#WORK[@]} -eq 0 ] && WORK=(--ratio=1:0 --key-pattern=R:R)
KEYS=${KEYS:-1000000}
TIME=${TIME:-30}
NET=bench-net SRV=bench-srv VOL=bench-data
BENCH_DIR=$(cd "$(dirname "$0")" && (pwd -W 2>/dev/null || pwd))
docker network create $NET >/dev/null 2>&1
docker rm -f $SRV >/dev/null 2>&1
docker volume rm $VOL >/dev/null 2>&1
docker run -d --name $SRV --network $NET --memory 2g --memory-swap 2g -v $VOL:/var/lib/datadam "$IMAGE" \
  --io-threads 8 --memcache-port 0 --cache-max-memory 1gb --cache-min-memory 64mb --rocksdb-block-cache 256mb \
  ${EXTRA:-} >/dev/null
for _ in $(seq 100); do
  docker run --rm --network $NET redis:7.4 redis-cli -h $SRV PING 2>/dev/null | grep -q PONG && break
  sleep 0.3
done
docker run --rm --network $NET -v "$BENCH_DIR":/bench:ro python:3.12-slim python /bench/load_random.py $SRV 6379 "$KEYS" 512 4
sleep 10
docker run -d --name bench-perf --privileged --pid=container:$SRV perf-tools sh -c \
  "/usr/lib/linux-tools/*/perf record -F 999 -g -p 1 -o /tmp/perf.data -- sleep $((TIME - 5)) >/dev/null 2>&1; \
   /usr/lib/linux-tools/*/perf report -i /tmp/perf.data --no-children --sort symbol --stdio 2>/dev/null | \
   grep -E '^ +[0-9.]+%' | head -45" >/dev/null
sleep 2
docker run --rm --network $NET redislabs/memtier_benchmark:latest -s $SRV -p 6379 --protocol=redis --threads=4 \
  --clients=25 --data-size=512 --random-data --key-minimum=1 --key-maximum="$KEYS" --hide-histogram \
  --distinct-client-seed --print-percentiles=50,99,99.9 --test-time="$TIME" "${WORK[@]}" 2>/dev/null | grep -E '^(Totals)'
docker wait bench-perf >/dev/null
docker logs bench-perf
docker rm -f bench-perf $SRV >/dev/null 2>&1
docker volume rm $VOL >/dev/null 2>&1
