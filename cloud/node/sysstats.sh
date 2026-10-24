#!/bin/bash
# CPU and disk statistics of this node, once a second, while a workload runs.
#   sysstats.sh start <name>   |   sysstats.sh stop   (stop uploads them)
. /opt/datadam/cloud/node/common.sh

case ${1:?usage: sysstats.sh start <name> | stop} in
  start)
    name=${2:?name}
    pkill -x mpstat 2>/dev/null || true
    pkill -x iostat 2>/dev/null || true
    nohup mpstat 1 > "$RESULTS/$name-$NODE-mpstat.txt" 2>&1 &
    nohup iostat -dxm 1 > "$RESULTS/$name-$NODE-iostat.txt" 2>&1 &
    ;;
  stop)
    pkill -x mpstat 2>/dev/null || true
    pkill -x iostat 2>/dev/null || true
    upload
    ;;
esac
