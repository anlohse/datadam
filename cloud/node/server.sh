#!/bin/bash
# On a server node: runs one store at a time, on a fresh data directory, sized
# for the machine (extra arguments override the sizes).
#   server.sh start datadam|redis|kvrocks [server arguments...]
#   server.sh stop
#   server.sh info [section]
. /opt/datadam/cloud/node/common.sh

cmd=${1:?usage: server.sh start <datadam|redis|kvrocks> [args...] | stop | info [section]}
shift

mem_mb=$(($(awk '/MemTotal/ {print $2}' /proc/meminfo) / 1024))
cpus=$(nproc)

case $cmd in
  start)
    kind=${1:?which server}
    shift
    docker rm -f bench-server >/dev/null 2>&1 || true
    rm -rf "/data/$kind"
    mkdir -p "/data/$kind"
    chmod 777 "/data/$kind"
    run=(docker run -d --name bench-server --network host --ulimit nofile=1048576:1048576)
    case $kind in
      datadam)
        # Caches about 60% of the memory: 45% objects, 15% RocksDB blocks.
        "${run[@]}" -v /data/datadam:/var/lib/datadam "$IMAGE" \
          --requirepass "$PASSWORD" --memcache-port 0 --io-threads "$cpus" \
          --cache-max-memory "$((mem_mb * 45 / 100))mb" --rocksdb-block-cache "$((mem_mb * 15 / 100))mb" \
          --rocksdb-write-buffer 128mb --rocksdb-background-jobs "$((cpus > 4 ? cpus / 2 : 2))" "$@" >/dev/null
        ;;
      redis)
        # Everything in memory: maxmemory under the machine's, no eviction (a
        # database must not drop data), AOF like the others' WAL.
        "${run[@]}" -v /data/redis:/data redis:7.4 redis-server --port 6379 --requirepass "$PASSWORD" \
          --appendonly yes --appendfsync everysec --save '' --maxmemory "$((mem_mb * 80 / 100))mb" \
          --maxmemory-policy noeviction --io-threads "$((cpus > 8 ? 8 : cpus))" --io-threads-do-reads yes "$@" >/dev/null
        ;;
      kvrocks)
        # Not over /var/lib/kvrocks: the image keeps its kvrocks.conf there.
        "${run[@]}" -v /data/kvrocks:/kvrocks-data apache/kvrocks:latest --bind 0.0.0.0 --port 6379 \
          --requirepass "$PASSWORD" --dir /kvrocks-data --workers "$cpus" \
          --rocksdb.block_cache_size "$((mem_mb * 60 / 100))" "$@" >/dev/null
        ;;
      *) echo "unknown server $kind" >&2; exit 2 ;;
    esac
    wait_ping 127.0.0.1
    echo "$kind started ($cpus CPUs, $mem_mb MB)"
    ;;
  stop)
    docker rm -f bench-server >/dev/null 2>&1 || true
    ;;
  info)
    rcli 127.0.0.1 INFO "${1:-everything}"
    ;;
  *) echo "unknown command $cmd" >&2; exit 2 ;;
esac
