#!/bin/bash
# Phase 5 (high availability), on the loader:
#   ha_load.sh write <name> <seconds> [workers]   writes acknowledged by WAIT 1 (bench/ha_check.py)
#   ha_load.sh verify <name>                      are they all on every data node?
#   ha_load.sh primary                            the primary's address, as the sentinels say
#   ha_load.sh failover                           a planned failover (SENTINEL FAILOVER)
#   ha_load.sh ready <replicas>                   waits until every sentinel knows the
#                                                 replicas and the other sentinels
. /opt/datadam/cloud/node/common.sh

cmd=${1:?usage: ha_load.sh write <name> <seconds> [workers] | verify <name> | primary | failover | ready <replicas>}
shift
data="" sentinels=""
for e in $NODES; do
  IFS='=' read -r n r ip <<< "$e"
  case $r in
    server | replica) data+="${data:+,}$n=$ip" ;;
    sentinel) sentinels+="${sentinels:+,}$ip:26379" ;;
  esac
done
sentinel_at() { local h=$1; shift; docker run --rm --network host redis:7.4 redis-cli -h "$h" -p 26379 "$@"; }
sentinel() { sentinel_at "${sentinels%%:*}" "$@"; }

check() {  # write|verify name [args]
  docker run --rm --network host -e REDIS_PASSWORD="$PASSWORD" -e SENTINELS="$sentinels" -e NODES="$data" \
    -e RUN="$2" -v /opt/datadam/bench:/bench:ro -v "$RESULTS":/out python:3.12-slim \
    python /bench/ha_check.py "$1" "/out/ha-$2" "${@:3}"
}

case $cmd in
  write)
    echo "writes from $(date +%s%3N)"
    check write "${1:?name}" "${2:?seconds}" "${3:-8}"
    upload
    ;;
  verify)
    check verify "${1:?name}"
    upload
    ;;
  primary) sentinel SENTINEL get-master-addr-by-name dd | head -1 ;;
  failover)
    echo "failover asked at $(date +%s%3N)"
    sentinel SENTINEL FAILOVER dd
    ;;
  ready)
    want=${1:?replicas}
    others=$(( $(tr ',' '\n' <<< "$sentinels" | wc -l) - 1 ))
    for _ in $(seq 120); do
      ok=1
      for s in ${sentinels//,/ }; do
        r=$(sentinel_at "${s%%:*}" SENTINEL replicas dd 2>/dev/null | grep -c '^ip$' || true)
        n=$(sentinel_at "${s%%:*}" SENTINEL sentinels dd 2>/dev/null | grep -c '^ip$' || true)
        { [ "$r" -ge "$want" ] && [ "$n" -ge "$others" ]; } || ok=0
      done
      [ $ok = 1 ] && { echo "sentinels ready at $(date +%s%3N)"; exit 0; }
      sleep 1
    done
    echo "the sentinels do not know every replica and sentinel after 2 minutes" >&2
    exit 1
    ;;
  *) echo "unknown command $cmd" >&2; exit 2 ;;
esac
