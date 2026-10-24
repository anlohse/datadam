#!/bin/bash
# On the loader: one memtier_benchmark run against the server node, saved as
# <name>.json / <name>.txt with the server's INFO before and after, and uploaded.
#   load.sh <name> [memtier arguments...]
. /opt/datadam/cloud/node/common.sh

name=${1:?usage: load.sh <name> [memtier args...]}
shift
server=$(first_of_role server)

rcli "$server" CONFIG RESETSTAT >/dev/null 2>&1 || true
rcli "$server" INFO everything > "$RESULTS/$name-info-before.txt" 2>&1 || true
docker run --rm --network host -v "$RESULTS":/out redislabs/memtier_benchmark:latest \
  -s "$server" -p 6379 --authenticate="$PASSWORD" --protocol=redis --hide-histogram \
  --print-percentiles=50,99,99.9,99.99 --json-out-file="/out/$name.json" "$@" > "$RESULTS/$name.txt" 2>&1
rcli "$server" INFO everything > "$RESULTS/$name-info-after.txt" 2>&1 || true
grep -E '^(Type|Sets|Gets|Totals) ' "$RESULTS/$name.txt"
upload
