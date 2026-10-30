#!/usr/bin/env bash
# Short versions of the cloud test's fault phases (cloud/README.md), against
# what they found:
#   crash     datadam killed (kill -9) twice under numbered writes of every
#             type (bench/crash_check.py): no acknowledged write lost, what
#             survived a prefix, collections consistent; and the live WAL
#             bounded (it once grew to 10 GB).
#   failover  a primary, two replicas and three sentinels; the primary killed
#             under writes acknowledged by WAIT 1 (bench/ha_check.py): none
#             lost, the other replica continues without a full sync (it once
#             synced in full after every failover) with its link up (a stale
#             link once reported it down for good), the old primary comes back
#             as a replica; and WAIT does not slow down as the WAL grows (it
#             once sought the WAL from scratch for every batch). Then a planned
#             SENTINEL FAILOVER with promotion-handoff: none lost, no full sync
#             (the old primary went on taking writes the promoted replica
#             never got: ~13,000 acknowledged writes lost in the cloud test).
#
# usage: bash tests/docker_fault_test.sh [crash|failover]   (needs the image datadam:latest)
set -u
export MSYS_NO_PATHCONV=1
IMAGE=${IMAGE:-datadam:latest}
NET=dd-fault-net
VOL=dd-fault-data
ONLY=${1:-}
BENCH=$(cd "$(dirname "$0")/../bench" && (pwd -W 2>/dev/null || pwd))
OUT=$(mktemp -d)
OUT_HOST=$(cd "$OUT" && (pwd -W 2>/dev/null || pwd))
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
field() { cli -h "$1" INFO "$2" | sed -n "s/^$3://p"; }  # host section field
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
wait_ping() {
  for _ in $(seq 120); do
    [ "$(cli -h "$1" PING)" = PONG ] && return 0
    sleep 0.5
  done
  echo "no PONG from $1"
  return 1
}
check() {  # crash|ha, then the checker's arguments; env: E (docker -e options)
  local tool=$1
  shift
  # shellcheck disable=SC2086
  docker run --rm --network $NET $E -v "$BENCH":/bench:ro -v "$OUT_HOST":/out python:3.12-slim \
    python "/bench/${tool}_check.py" "$@"
}
cleanup() {
  docker rm -f dd-crash dd-p dd-r1 dd-r2 dd-s1 dd-s2 dd-s3 >/dev/null 2>&1
  docker volume rm $VOL >/dev/null 2>&1
  docker network rm $NET >/dev/null 2>&1
}
cleanup
docker network create $NET >/dev/null

crash() {
  echo "== crash: kill -9 under writes"
  docker volume create $VOL >/dev/null
  # Small memtables: the live WAL's bound, 8 of them, is 32 MB.
  docker run -d --name dd-crash --network $NET -v $VOL:/var/lib/datadam "$IMAGE" --memcache-port 0 \
    --io-threads 2 --rocksdb-write-buffer 4mb >/dev/null
  wait_ping dd-crash
  for round in 1 2; do
    E="-e HOST=dd-crash -e RUN=crash$round" check crash write "/out/crash-$round" 20 4 16 2 > "$OUT/write-$round.txt" 2>&1 &
    writer=$!
    sleep 6
    if [ "$round" = 1 ]; then
      # Hundreds of MB written by now; left to RocksDB, all of it would still be live WAL.
      wal=$(field dd-crash persistence rocksdb_live_wal_bytes)
      [ -n "$wal" ] && [ "$wal" -lt $((64 << 20)) ] && report "the live WAL stays bounded under writes" 1 ||
        report "the live WAL stays bounded under writes" 0 "rocksdb_live_wal_bytes $wal"
    fi
    sleep 2
    docker kill -s KILL dd-crash >/dev/null
    wait $writer
    docker start dd-crash >/dev/null
    wait_ping dd-crash
    if E="-e HOST=dd-crash -e RUN=crash$round" check crash verify "/out/crash-$round"; then
      report "round $round: every acknowledged write is there, a prefix, consistent" 1
    else
      report "round $round: every acknowledged write is there, a prefix, consistent" 0 "see above"
      tail -3 "$OUT/write-$round.txt"
    fi
  done
  E="-e HOST=dd-crash -e RUN=crash1" check crash verify /out/crash-1 final >/dev/null &&
    report "round 1's writes outlived the second crash" 1 ||
    report "round 1's writes outlived the second crash" 0 "verify-final failed"
  docker rm -f dd-crash >/dev/null
}

failover() {
  echo "== failover: the primary killed under WAIT 1"
  docker run -d --name dd-p --network $NET "$IMAGE" --memcache-port 0 --io-threads 2 --wal-retention 1h >/dev/null
  wait_ping dd-p
  local P R1 R2
  P=$(ip_of dd-p)
  for r in dd-r1 dd-r2; do
    docker run -d --name $r --network $NET "$IMAGE" --memcache-port 0 --io-threads 2 --wal-retention 1h \
      --replicaof "$P 6379" >/dev/null
  done
  R1=$(ip_of dd-r1)
  R2=$(ip_of dd-r2)
  local sentinels=""
  for s in dd-s1 dd-s2 dd-s3; do
    docker run -d --name $s --network $NET redis:7.4 sh -c "printf '%s\n' 'port 26379' \
      'sentinel monitor dd $P 6379 2' 'sentinel down-after-milliseconds dd 2000' \
      'sentinel failover-timeout dd 10000' 'sentinel parallel-syncs dd 1' > /tmp/s.conf; \
      exec redis-sentinel /tmp/s.conf" >/dev/null
    sentinels+="${sentinels:+,}$(ip_of $s):26379"
  done
  until_true "sentinels see both replicas" 40 \
    '[ "$(cli -h dd-s1 -p 26379 SENTINEL replicas dd | grep -c "^ip$")" -ge 2 ]'
  until_true "sentinels see each other" 40 \
    '[ "$(cli -h dd-s1 -p 26379 SENTINEL sentinels dd | grep -c "^ip$")" -ge 2 ]'
  E="-e SENTINELS=$sentinels -e NODES=p=$P,r1=$R1,r2=$R2 -e RUN=ci -e STRICT=1 -e SYNC_LIMIT_S=120"

  # WAIT's time must not grow with the WAL behind it.
  local before after
  before=$(check ha wait-latency 300 | sed -n 's/.*"p50_ms": \([0-9.]*\).*/\1/p')
  docker run --rm --network $NET redis:7.4 redis-benchmark -h "$P" -t set -n 100000 -d 1000 -r 100000 \
    -c 20 -P 16 -q >/dev/null 2>&1
  after=$(check ha wait-latency 300 | sed -n 's/.*"p50_ms": \([0-9.]*\).*/\1/p')
  if [ -n "$before" ] && [ -n "$after" ] && awk "BEGIN { exit !($after <= 4 * $before + 1) }"; then
    report "WAIT takes as long with 100 MB of WAL as with none (${before} -> ${after} ms)" 1
  else
    report "WAIT takes as long with 100 MB of WAL as with none" 0 "p50 ${before:-?} -> ${after:-?} ms"
  fi

  check ha write /out/ha 25 4 > "$OUT/ha-write.txt" 2>&1 &
  local writer=$!
  sleep 8
  docker kill -s KILL dd-p >/dev/null
  wait $writer
  local primary other
  primary=$(cli -h dd-s1 -p 26379 SENTINEL get-master-addr-by-name dd | head -1)
  [ "$primary" = "$R1" ] && other=$R2 || other=$R1
  [ "$primary" = "$R1" ] || [ "$primary" = "$R2" ] && report "a replica was promoted ($primary)" 1 ||
    report "a replica was promoted" 0 "the sentinels name $primary"
  if check ha verify /out/ha; then
    report "no write acknowledged with WAIT 1 was lost, the replicas back in sync" 1
  else
    report "no write acknowledged with WAIT 1 was lost, the replicas back in sync" 0 "see above (not_in_sync: a replica that never caught up)"
    tail -3 "$OUT/ha-write.txt"
  fi
  local full partial
  full=$(field "$other" replication full_syncs)
  partial=$(field "$other" replication partial_syncs)
  [ "$full" = 1 ] && [ "${partial:-0}" -ge 1 ] &&
    report "the other replica continued from the promoted one, no full sync" 1 ||
    report "the other replica continued from the promoted one, no full sync" 0 "full_syncs $full, partial_syncs $partial"
  sleep 3  # past an acknowledgment tick: a stale link would have reported itself down by now
  [ "$(field "$other" replication master_link_status)" = up ] && report "its link stays up" 1 ||
    report "its link stays up" 0 "$(cli -h "$other" INFO replication | grep -E 'link|down')"

  docker start dd-p >/dev/null
  local old
  old=$(ip_of dd-p)
  until_true "the old primary comes back as a replica, in sync" 60 \
    '[ "$(field $old replication master_link_status)" = up ] &&
     [ "$(field $old replication slave_repl_offset)" = "$(field $primary replication master_repl_offset)" ]'

  echo "== failover: SENTINEL FAILOVER under WAIT 1, with promotion-handoff"
  # Without the hand-over the old primary went on taking writes, acknowledged
  # by its other replica, that the promoted one never got: lost, and both
  # synced in full.
  local n fulls_before fulls_after
  for n in "$old" "$R1" "$R2"; do cli -h "$n" CONFIG SET promotion-handoff yes >/dev/null; done
  # Full syncs served by any of them (a primary shows no full_syncs of its own).
  fulls() { for n in "$old" "$R1" "$R2"; do field "$n" replication full_syncs_served; done | awk '{ n += $1 } END { print n }'; }
  fulls_before=$(fulls)
  E="-e SENTINELS=$sentinels -e NODES=p=$old,r1=$R1,r2=$R2 -e RUN=ci-planned -e STRICT=1 -e SYNC_LIMIT_S=120"
  check ha write /out/ha-planned 20 4 > "$OUT/ha-planned-write.txt" 2>&1 &
  writer=$!
  sleep 6
  cli -h dd-s1 -p 26379 SENTINEL FAILOVER dd >/dev/null
  wait $writer
  local promoted
  promoted=$(cli -h dd-s1 -p 26379 SENTINEL get-master-addr-by-name dd | head -1)
  [ -n "$promoted" ] && [ "$promoted" != "$primary" ] && report "Sentinel moved the primary ($primary -> $promoted)" 1 ||
    report "Sentinel moved the primary" 0 "the sentinels name $promoted"
  if check ha verify /out/ha-planned; then
    report "no write acknowledged with WAIT 1 was lost in the planned failover" 1
  else
    report "no write acknowledged with WAIT 1 was lost in the planned failover" 0 "see above"
    tail -3 "$OUT/ha-planned-write.txt"
  fi
  fulls_after=$(fulls)
  [ "$fulls_after" = "$fulls_before" ] && report "no server synced in full" 1 ||
    report "no server synced in full" 0 "full syncs served: $fulls_before -> $fulls_after"
}

[ -z "$ONLY" ] || [ "$ONLY" = crash ] && crash
[ -z "$ONLY" ] || [ "$ONLY" = failover ] && failover

echo "faults: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then
  for c in dd-crash dd-p dd-r1 dd-r2; do
    docker inspect $c >/dev/null 2>&1 && { echo "--- $c"; docker logs $c 2>&1 | tail -15; }
  done
fi
cleanup
rm -rf "$OUT"
[ "$fail" -eq 0 ]
