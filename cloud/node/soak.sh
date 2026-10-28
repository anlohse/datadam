#!/bin/bash
# Phase 7 (soak), on the server:
#   soak.sh start [interval s]    datadam on a fresh data directory, with /metrics, and a
#                                 sampler in the background (systemd unit soak-sampler)
#   soak.sh sample <interval s>   (the unit) /metrics, disk use and load every interval,
#                                 into $RESULTS/soak; uploaded every 10 minutes
#   soak.sh stop                  stops the sampler; a last INFO; uploads
. /opt/datadam/cloud/node/common.sh

cmd=${1:?usage: soak.sh start [interval s] | sample <interval s> | stop}
shift
dir=$RESULTS/soak
mkdir -p "$dir"

case $cmd in
  start)
    interval=${1:-60}
    /opt/datadam/cloud/node/server.sh start datadam --metrics-port 9121
    systemctl stop soak-sampler 2>/dev/null || true
    systemd-run --unit=soak-sampler --collect /opt/datadam/cloud/node/soak.sh sample "$interval" >/dev/null
    echo "sampling every $interval s"
    ;;
  sample)
    interval=${1:?interval}
    every=$(( interval >= 600 ? 1 : 600 / interval ))
    n=0
    while true; do
      t=$(date +%s%3N)
      { echo "# t $t"; curl -s --max-time 10 http://127.0.0.1:9121/metrics || true; } >> "$dir/metrics.txt"
      echo "$t $(du -sb /data/datadam 2>/dev/null | cut -f1) $(cut -d' ' -f1-3 /proc/loadavg)" >> "$dir/disk.txt"
      n=$((n + 1))
      if (( n % every == 0 )); then upload || true; fi
      sleep "$interval"
    done
    ;;
  stop)
    systemctl stop soak-sampler 2>/dev/null || true
    rcli 127.0.0.1 INFO everything > "$dir/info-end.txt" 2>&1 || true
    upload
    ;;
  *) echo "unknown command $cmd" >&2; exit 2 ;;
esac
