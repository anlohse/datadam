#!/bin/bash
# Phase 5 (high availability), on a data node (the server or a replica):
#   ha.sh primary                  datadam as the primary, on a fresh data directory
#   ha.sh kill                     kill -9 the running datadam (prints the time, unix ms)
#   ha.sh cut <ip>...              drop all traffic to and from these addresses
#   ha.sh heal                     undo cut
#   ha.sh restart                  restart datadam, keeping its data (a replica catches up)
#   ha.sh rejoin <primary ip>      datadam again, empty, as a replica of the primary;
#                                  waits until it is in sync and prints how long it took
#   ha.sh synced <primary ip>      waits until this replica is in sync with the primary
# Every datadam here keeps the WAL an hour (a replica can catch up, a promoted
# one can feed the others) and serves /metrics on 9121.
. /opt/datadam/cloud/node/common.sh

cmd=${1:?usage: ha.sh primary | kill | cut <ip>... | heal | restart | rejoin <primary ip>}
shift
ms() { date +%s%3N; }

run_datadam() {  # [args...]
  docker rm -f bench-server >/dev/null 2>&1 || true
  rm -rf /data/datadam
  mkdir -p /data/datadam
  chmod 777 /data/datadam
  docker run -d --name bench-server --network host --ulimit nofile=1048576:1048576 \
    -v /data/datadam:/var/lib/datadam "$IMAGE" --requirepass "$PASSWORD" --masterauth "$PASSWORD" \
    --memcache-port 0 --wal-retention 1h --metrics-port 9121 "$@" >/dev/null
}

info_field() {  # host field ("" when the host does not answer)
  { rcli "$1" INFO replication 2>/dev/null || true; } | tr -d '\r' | sed -n "s/^$2://p"
}

case $cmd in
  primary)
    run_datadam
    wait_ping 127.0.0.1
    echo "primary started at $(ms)"
    ;;
  kill)
    echo "killed at $(ms)"
    docker kill -s KILL bench-server >/dev/null
    ;;
  cut)
    for ip in "$@"; do
      iptables -I INPUT -s "$ip" -j DROP
      iptables -I OUTPUT -d "$ip" -j DROP
      echo "$ip" >> /run/ha-cut
    done
    echo "cut at $(ms) from $*"
    ;;
  heal)
    if [ -f /run/ha-cut ]; then
      while read -r ip; do
        iptables -D INPUT -s "$ip" -j DROP || true
        iptables -D OUTPUT -d "$ip" -j DROP || true
      done < /run/ha-cut
      rm -f /run/ha-cut
    fi
    echo "healed at $(ms)"
    ;;
  restart)
    echo "restart at $(ms)"
    docker restart bench-server >/dev/null
    ;;
  rejoin)
    primary=${1:?primary ip}
    start=$(ms)
    run_datadam
    wait_ping 127.0.0.1
    # REPLICAOF, written to the config, not --replicaof: the command line would
    # win over what Sentinel writes there, and a restart would follow a stale primary.
    rcli 127.0.0.1 REPLICAOF "$primary" 6379 >/dev/null
    rcli 127.0.0.1 CONFIG REWRITE >/dev/null
    "$0" synced "$primary" "$start"
    ;;
  synced)
    # Waits until this node is a replica of the primary, its link up, no sync
    # under way, and its offset within 1000 of the primary's; prints how long
    # that took (from <since>, unix ms, default now).
    primary=${1:?primary ip}
    start=${2:-$(ms)}
    for _ in $(seq 7200); do
      if [ "$(info_field 127.0.0.1 role)" = slave ] && [ "$(info_field 127.0.0.1 master_host)" = "$primary" ] &&
         [ "$(info_field 127.0.0.1 master_link_status)" = up ] && [ "$(info_field 127.0.0.1 master_sync_in_progress)" = 0 ]; then
        target=$(info_field "$primary" master_repl_offset)
        mine=$(info_field 127.0.0.1 slave_repl_offset)
        if [ -n "$target" ] && [ -n "$mine" ] && [ "$mine" -ge "$((target - 1000))" ]; then
          echo "in sync in $(( $(ms) - start )) ms (offset $mine of $target, $(rcli 127.0.0.1 DBSIZE) keys," \
               "full syncs $(info_field 127.0.0.1 full_syncs), partial $(info_field 127.0.0.1 partial_syncs))"
          exit 0
        fi
      fi
      sleep 0.5
    done
    echo "not in sync with $primary after an hour" >&2
    exit 1
    ;;
  *) echo "unknown command $cmd" >&2; exit 2 ;;
esac
