#!/bin/bash
# On the loader: fills the server with memtier-1 .. memtier-<keys>, values of
# <bytes> truly random bytes (bench/load_random.py), and records how long it took.
#   dataset.sh <name> <keys> [bytes, default 512]
. /opt/datadam/cloud/node/common.sh

name=${1:?usage: dataset.sh <name> <keys> [bytes]}
keys=${2:?keys}
bytes=${3:-512}
server=$(first_of_role server)
result=$(docker run --rm --network host -e REDIS_PASSWORD="$PASSWORD" -v /opt/datadam/bench:/bench:ro \
  python:3.12-slim python /bench/load_random.py "$server" 6379 "$keys" "$bytes" 8)
echo "$result" | tee "$RESULTS/$name-load.txt"
# Let the memtables flush before anything is measured.
sleep 10
