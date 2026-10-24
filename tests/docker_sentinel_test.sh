#!/usr/bin/env bash
# Automatic failover of datadam by Redis Sentinel: a primary, a replica and
# three sentinels (redis:7.4's redis-sentinel) in Docker. The primary is
# stopped; the sentinels must promote the replica, and once the old primary
# comes back, make it a replica of the new one.
#
# usage: bash tests/docker_sentinel_test.sh     (needs the image datadam:latest)
set -u
export MSYS_NO_PATHCONV=1
NET=dd-sentinel-net
IMAGE=${IMAGE:-datadam:latest}
pass=0
fail=0
report() {  # name, ok?, detail
  if [ "$2" = 1 ]; then
    pass=$((pass + 1))
    echo "  ok   $1"
  else
    fail=$((fail + 1))
    echo "  FAIL $1: $3"
  fi
}
cli() { docker run --rm --network $NET redis:7.4 redis-cli "$@" 2>&1 | tr -d '\r'; }
ip_of() { docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$1"; }
# until NAME SECONDS CONDITION...: retries the condition once a second.
until_true() {
  local name=$1 seconds=$2
  shift 2
  for _ in $(seq "$seconds"); do
    if eval "$@" >/dev/null 2>&1; then
      report "$name" 1
      return 0
    fi
    sleep 1
  done
  report "$name" 0 "not true after ${seconds}s: $*"
  return 1
}
cleanup() { docker rm -f dd-primary dd-replica dd-s1 dd-s2 dd-s3 >/dev/null 2>&1; docker network rm $NET >/dev/null 2>&1; }
cleanup
docker network create $NET >/dev/null

docker run -d --name dd-primary --network $NET "$IMAGE" --memcache-port 0 --wal-retention 1h >/dev/null
sleep 1
PRIMARY=$(ip_of dd-primary)
docker run -d --name dd-replica --network $NET "$IMAGE" --memcache-port 0 --wal-retention 1h \
  --replicaof "$PRIMARY 6379" >/dev/null
sleep 1
REPLICA=$(ip_of dd-replica)
echo "primary $PRIMARY, replica $REPLICA"

for s in dd-s1 dd-s2 dd-s3; do
  docker run -d --name $s --network $NET redis:7.4 sh -c "printf '%s\n' \
    'port 26379' \
    'sentinel monitor mymaster $PRIMARY 6379 2' \
    'sentinel down-after-milliseconds mymaster 2000' \
    'sentinel failover-timeout mymaster 10000' \
    'sentinel parallel-syncs mymaster 1' > /tmp/s.conf; exec redis-sentinel /tmp/s.conf" >/dev/null
done

echo "== discovery"
cli -h "$PRIMARY" SET before failover >/dev/null
until_true "replica has the data" 15 '[ "$(cli -h $REPLICA GET before)" = failover ]'
until_true "sentinels see the replica" 30 'cli -h dd-s1 -p 26379 SENTINEL replicas mymaster | grep -qx "$REPLICA"'
until_true "sentinels see each other" 30 \
  '[ "$(cli -h dd-s1 -p 26379 SENTINEL sentinels mymaster | grep -c "^ip$")" -ge 2 ]'

echo "== the primary goes down"
docker stop dd-primary >/dev/null
until_true "sentinels promote the replica" 60 \
  '[ "$(cli -h dd-s1 -p 26379 SENTINEL get-master-addr-by-name mymaster | head -1)" = "$REPLICA" ]'
until_true "the replica is a primary" 15 'cli -h $REPLICA ROLE | head -1 | grep -qx master'
r=$(cli -h "$REPLICA" SET after failover)
[ "$r" = OK ] && report "the new primary takes writes" 1 || report "the new primary takes writes" 0 "$r"
r=$(cli -h "$REPLICA" GET before)
[ "$r" = failover ] && report "the data is there" 1 || report "the data is there" 0 "$r"

echo "== the old primary comes back"
docker start dd-primary >/dev/null
OLD=$(ip_of dd-primary)
until_true "sentinels make it a replica" 60 'cli -h $OLD ROLE | head -1 | grep -qx slave'
until_true "it follows the new primary" 30 '[ "$(cli -h $OLD GET after)" = failover ]'
r=$(cli -h "$OLD" SET x 1)
[[ "$r" == READONLY* ]] && report "it refuses writes" 1 || report "it refuses writes" 0 "$r"

echo "sentinel: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then
  echo "--- sentinel log"
  docker logs dd-s1 2>&1 | tail -30
  echo "--- replica log"
  docker logs dd-replica 2>&1 | tail -20
fi
cleanup
[ "$fail" -eq 0 ]
