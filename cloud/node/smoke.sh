#!/bin/bash
# Phase 1 (smoke), on the loader: the server answers, refuses clients without
# the password, takes a short load, and the results reach the bucket.
. /opt/datadam/cloud/node/common.sh

server=$(first_of_role server)
check() {  # name, expected substring, actual
  if [[ "$3" == *"$2"* ]]; then echo "ok   $1"; else echo "FAIL $1: expected *$2*, got: $3"; exit 1; fi
}

wait_ping "$server"
check "PING" "PONG" "$(rcli "$server" PING)"
check "SET / GET" "ok" "$(rcli "$server" SET smoke ok >/dev/null; rcli "$server" GET smoke)"
check "no password: refused" "NOAUTH" \
  "$(docker run --rm --network host redis:7.4 redis-cli -h "$server" PING 2>&1)"
check "version" "redis_version:7.2.0" "$(rcli "$server" INFO server)"

docker run --rm --network host redis:7.4 redis-benchmark -h "$server" -a "$PASSWORD" -q -n 200000 -c 50 -P 16 \
  -r 100000 -t set,get 2>&1 | tr '\r' '\n' | grep 'per second' | tee "$RESULTS/smoke-redis-benchmark.txt"
/opt/datadam/cloud/node/load.sh smoke-mixed --threads=2 --clients=25 --test-time=20 --ratio=1:4 --data-size=512 \
  --key-maximum=100000 --random-data --distinct-client-seed
echo "smoke passed"
