#!/usr/bin/env bash
# memtier_benchmark: datadam vs Redis vs Kvrocks, each in a container with a
# hard memory limit (no swap), on a fresh volume, one server at a time.
#
# usage: bench/memtier.sh <fit|big> [server ...]      servers: datadam redis kvrocks
#   fit  1M keys x 512 B   (~0.5 GB of values) in 2 GB containers: fits in memory
#   big  6M keys x 512 B   (~3 GB of values)   in 1 GB containers: 3x the memory
# env: KEYS, DATA (value bytes), MEM (container limit), TIME (seconds per workload),
#      SEEDS: "same" (memtier's default: every client draws the same key sequence,
#      so the 100 clients ask for each key at about the same moment) or
#      "distinct" (--distinct-client-seed: independent clients, the realistic case)
#      DATADAM_IMAGE (default datadam:latest), to compare two builds
#
# The data set is loaded by bench/load_random.py with truly random values:
# memtier's own --random-data reuses one buffer for every key (89x compressible
# with gzip), which a compressing store would shrink to a fraction of memory.
# memtier then runs the workloads. Its writes are those compressible values
# again, so every workload that writes starts from a freshly loaded data set.
# Results (memtier JSON and text, resource notes) go to bench/results/<scenario>-<time>/;
# bench/report.py turns them into tables.
set -uo pipefail
export MSYS_NO_PATHCONV=1  # Git Bash on Windows: keep container paths as they are

SCENARIO=${1:?usage: bench/memtier.sh <fit|big> [server ...]}
shift
SERVERS=("$@")
[ ${#SERVERS[@]} -eq 0 ] && SERVERS=(datadam redis kvrocks)
case $SCENARIO in
  fit) KEYS=${KEYS:-1000000} MEM=${MEM:-2g} ;;
  big) KEYS=${KEYS:-6000000} MEM=${MEM:-1g} ;;
  *) echo "unknown scenario $SCENARIO"; exit 2 ;;
esac
DATA=${DATA:-512}
SEEDS=${SEEDS:-distinct}
TIME=${TIME:-60}
THREADS=4
CLIENTS=25  # 100 connections

OUT=bench/results/$SCENARIO-$SEEDS-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
OUT_MOUNT=$(cd "$OUT" && (pwd -W 2>/dev/null || pwd))
NET=bench-net
SRV=bench-srv
VOL=bench-data
docker network create $NET >/dev/null 2>&1

# Memory budgets: about half of the container for caches, the rest for
# memtables, connections and the process itself.
server_cmd() {  # server -> image, data dir, port and arguments
  case "$1:$SCENARIO" in
    datadam:fit) echo "${DATADAM_IMAGE:-datadam:latest} /var/lib/datadam 6379 --io-threads 8 --memcache-port 0 --cache-max-memory 1gb --cache-min-memory 64mb --rocksdb-block-cache 256mb" ;;
    datadam:big) echo "${DATADAM_IMAGE:-datadam:latest} /var/lib/datadam 6379 --io-threads 8 --memcache-port 0 --cache-max-memory 384mb --cache-min-memory 64mb --rocksdb-block-cache 128mb --rocksdb-write-buffer 32mb" ;;
    kvrocks:fit) echo "apache/kvrocks:latest /var/lib/kvrocks 6666 --rocksdb.block_cache_size 1280" ;;
    kvrocks:big) echo "apache/kvrocks:latest /var/lib/kvrocks 6666 --rocksdb.block_cache_size 512 --rocksdb.write_buffer_size 32" ;;
    # Redis keeps everything in memory: maxmemory just under the container
    # limit, noeviction (a database must not drop data), AOF like the others'
    # WAL, and I/O threads (the others use 8 threads; Redis's default is one).
    redis:fit) echo "redis:7.4 /data 6379 redis-server --appendonly yes --appendfsync everysec --save '' --maxmemory 1800mb --maxmemory-policy noeviction --io-threads 4 --io-threads-do-reads yes" ;;
    redis:big) echo "redis:7.4 /data 6379 redis-server --appendonly yes --appendfsync everysec --save '' --maxmemory 900mb --maxmemory-policy noeviction --io-threads 4 --io-threads-do-reads yes" ;;
  esac
}

note() { echo "$1 $2 $3" >> "$OUT/notes.txt"; }

alive() { [ "$(docker inspect -f '{{.State.Running}}' $SRV 2>/dev/null)" = true ]; }

memtier() {  # server, port, run name, memtier args...
  local server=$1 port=$2 name=$3 seed=()
  shift 3
  [ "$SEEDS" = distinct ] && seed=(--distinct-client-seed)
  docker run --rm --network $NET -v "$OUT_MOUNT":/out redislabs/memtier_benchmark:latest \
    -s $SRV -p "$port" --protocol=redis --threads=$THREADS --clients=$CLIENTS \
    --data-size="$DATA" --random-data --key-minimum=1 --key-maximum="$KEYS" \
    --hide-histogram --print-percentiles=50,99,99.9 --json-out-file="/out/$server-$name.json" "${seed[@]}" "$@" \
    > "$OUT/$server-$name.txt" 2>&1
  grep -E '^(Totals|Sets|Gets) ' "$OUT/$server-$name.txt" | sed "s/^/  $name /"
}

resources() {  # server, moment
  local mem disk
  mem=$(docker stats --no-stream --format '{{.MemUsage}}' $SRV 2>/dev/null | cut -d/ -f1 | tr -d ' ')
  disk=$(docker exec $SRV sh -c "du -sm $DIR 2>/dev/null | cut -f1")
  note "$1" "mem_$2" "$mem"
  note "$1" "disk_mb_$2" "$disk"
  echo "  $2: memory $mem, disk ${disk} MB"
}

BENCH_DIR=$(cd "$(dirname "$0")" && (pwd -W 2>/dev/null || pwd))

start_server() {  # fresh container and volume
  docker rm -f $SRV >/dev/null 2>&1
  docker volume rm $VOL >/dev/null 2>&1
  eval "docker run -d --name $SRV --network $NET --memory $MEM --memory-swap $MEM -v $VOL:$DIR $IMAGE $ARGS" >/dev/null
  for _ in $(seq 100); do
    docker run --rm --network $NET redis:7.4 redis-cli -h $SRV -p "$PORT" PING 2>/dev/null | grep -q PONG && return
    sleep 0.3
  done
}

load_data() {  # server, label: every key once, random values, 4 pipelined connections
  local result
  result=$(docker run --rm --network $NET -v "$BENCH_DIR":/bench:ro python:3.12-slim \
    python /bench/load_random.py $SRV "$PORT" "$KEYS" "$DATA" 4)
  echo "  load ($2): $result"
  note "$1" "load_$2" "$result"
  sleep 10  # let memtables flush before measuring
}

for server in "${SERVERS[@]}"; do
  read -r IMAGE DIR PORT ARGS <<< "$(server_cmd "$server")"
  echo "=== $server ($SCENARIO: $KEYS keys x $DATA B, memory limit $MEM)"
  start_server
  load_data "$server" first
  resources "$server" after_load

  # Workloads, each for $TIME seconds, 100 connections, no pipelining unless said.
  #   G:G is a gaussian over the key range (a hot middle, about a third of the keys
  #   getting two thirds of the requests); R:R is uniform.
  for w in "read-uniform --ratio=0:1 --key-pattern=R:R" \
           "read-gaussian --ratio=0:1 --key-pattern=G:G" \
           "mixed-gaussian --ratio=1:4 --key-pattern=G:G" \
           "mixed-gaussian-p16 --ratio=1:4 --key-pattern=G:G --pipeline=16" \
           "write-uniform --ratio=1:0 --key-pattern=R:R"; do
    read -r name args <<< "$w"
    if [[ $name != read-* ]]; then  # writes: start again from incompressible data
      start_server
      load_data "$server" "$name"
    fi
    if ! alive; then
      echo "  server is not running (OOM killed: $(docker inspect -f '{{.State.OOMKilled}}' $SRV))"
      note "$server" stopped "oom_killed=$(docker inspect -f '{{.State.OOMKilled}}' $SRV)"
      break
    fi
    # shellcheck disable=SC2086
    memtier "$server" "$PORT" "$name" --test-time="$TIME" $args
  done
  alive && resources "$server" end
  note "$server" oom_killed "$(docker inspect -f '{{.State.OOMKilled}}' $SRV)"
  docker rm -f $SRV >/dev/null 2>&1
  docker volume rm $VOL >/dev/null 2>&1
done
echo "results in $OUT"
