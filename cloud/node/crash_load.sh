#!/bin/bash
# Phase 6 (crash), on the loader:
#   crash_load.sh write <name> <seconds> [workers] [pipeline] [churn]   numbered writes until the crash
#   crash_load.sh verify <name> [allow-loss] [final]                     is every acknowledged one there?
# (bench/crash_check.py; results in $RESULTS/crash-<name>)
. /opt/datadam/cloud/node/common.sh

cmd=${1:?usage: crash_load.sh write <name> <seconds> [workers] [pipeline] [churn] | verify <name> [allow-loss] [final]}
name=${2:?name}
shift 2
docker run --rm --network host -e REDIS_PASSWORD="$PASSWORD" -e HOST="$(target_server)" -e RUN="$name" \
  -v /opt/datadam/bench:/bench:ro -v "$RESULTS":/out python:3.12-slim \
  python /bench/crash_check.py "$cmd" "/out/crash-$name" "$@"
upload
