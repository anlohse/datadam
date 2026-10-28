#!/bin/bash
# Phase 7 (soak), on the loader:
#   soak_load.sh start <hours> <chunk s> <keys> <main rate> <ttl rate> <collections rate>
#       in the background (systemd unit soak-load): loads <keys> keys of 1 KB, then for
#       <hours> (fractions too) runs, at fixed rates (operations per second):
#       - main: memtier, 1 write for 4 reads, gaussian over the loaded keys, values of
#         100 to 4000 random bytes;
#       - ttl: memtier, SET/GET over 5M other keys that expire after 30 to 600 s;
#       - collections: bench/soak_collections.py, hashes, lists, sets and sorted sets
#         that grow and shrink;
#       memtier in chunks of <chunk s> (one result file each), uploaded after each.
#   soak_load.sh run ...      (the unit)
#   soak_load.sh status       chunks done, the last one's numbers, "done" at the end
. /opt/datadam/cloud/node/common.sh

cmd=${1:?usage: soak_load.sh start <hours> <chunk s> <keys> <main rate> <ttl rate> <collections rate> | status}
shift
dir=$RESULTS/soak
mkdir -p "$dir"

memtier() {  # name seconds [memtier arguments...]
  local name=$1 seconds=$2
  shift 2
  docker run --rm --network host -v "$dir":/out redislabs/memtier_benchmark:latest \
    -s "$(target_server)" -p 6379 --authenticate="$PASSWORD" --protocol=redis --hide-histogram \
    --print-percentiles=50,99,99.9 --test-time="$seconds" --json-out-file="/out/$name.json" "$@" > "$dir/$name.txt" 2>&1
  # memtier writes its command line, password included, into the JSON.
  sed -i -E 's/"authenticate": *"[^"]*"/"authenticate": "<redacted>"/' "$dir/$name.json" 2>/dev/null || true
}

case $cmd in
  start)
    systemctl stop soak-load 2>/dev/null || true
    docker rm -f soak-coll >/dev/null 2>&1 || true
    # A run before this one (a pilot) leaves its chunks here and in the bucket.
    rm -rf "$dir"
    aws s3 rm --recursive --only-show-errors --region "$REGION" "s3://$BUCKET/$RUN_ID/$NODE/soak/" || true
    systemd-run --unit=soak-load --collect /opt/datadam/cloud/node/soak_load.sh run "$@" >/dev/null
    echo "soak started: $*"
    ;;
  run)
    hours=${1:?hours} chunk=${2:?chunk s} keys=${3:?keys} main=${4:?main rate} ttl=${5:?ttl rate} coll=${6:?rate}
    mkdir -p "$dir"
    /opt/datadam/cloud/node/dataset.sh soak-preload "$keys" 1000 > "$dir/preload.txt" 2>&1
    end=$(( $(date +%s) + $(awk "BEGIN { print int($hours * 3600) }") ))
    docker rm -f soak-coll >/dev/null 2>&1 || true
    docker run -d --name soak-coll --network host -e REDIS_PASSWORD="$PASSWORD" -e HOST="$(target_server)" \
      -v /opt/datadam/bench:/bench:ro python:3.12-slim \
      python /bench/soak_collections.py "$(( end - $(date +%s) ))" "$coll" >/dev/null
    i=0
    while true; do
      left=$(( end - $(date +%s) ))
      (( left >= 30 )) || break
      i=$((i + 1))
      name=$(printf 'chunk-%04d' "$i")
      seconds=$(( left < chunk ? left : chunk ))
      # 200 connections for main, 20 for ttl: --rate-limiting is per connection.
      memtier "$name-main" "$seconds" --threads=4 --clients=50 --rate-limiting=$(( main / 200 )) --ratio=1:4 \
        --key-pattern=G:G --key-minimum=1 --key-maximum="$keys" --data-size-range=100-4000 --data-size-pattern=R \
        --random-data --distinct-client-seed &
      memtier "$name-ttl" "$seconds" --threads=1 --clients=20 --rate-limiting=$(( ttl / 20 )) --ratio=1:1 \
        --key-prefix=ttl- --key-pattern=R:R --key-minimum=1 --key-maximum=5000000 --expiry-range=30-600 \
        --data-size-range=100-2000 --random-data --distinct-client-seed &
      wait
      docker logs soak-coll > "$dir/collections.txt" 2>&1 || true
      upload || true
    done
    docker logs soak-coll > "$dir/collections.txt" 2>&1 || true
    docker rm -f soak-coll >/dev/null 2>&1 || true
    date +%s%3N > "$dir/done"
    upload
    ;;
  status)
    chunks=$(ls "$dir" 2>/dev/null | grep -c -- '-main.json$' || true)
    echo "chunks done: $chunks"
    last=$(ls "$dir" 2>/dev/null | grep -- '-main.txt$' | tail -1 || true)
    [ -n "$last" ] && grep -E '^(Sets|Gets|Totals) ' "$dir/$last" || true
    tail -1 "$dir/collections.txt" 2>/dev/null || true
    [ -f "$dir/done" ] && echo "done"
    systemctl is-active soak-load 2>/dev/null || true
    ;;
  *) echo "unknown command $cmd" >&2; exit 2 ;;
esac
