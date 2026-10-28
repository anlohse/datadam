#!/bin/bash
# Phase 6 (crash), on the server:
#   crash.sh start              datadam on a fresh data directory (server.sh start datadam)
#   crash.sh fsync <policy>     CONFIG SET fsync always|everysec|no
#   crash.sh kill               kill -9 datadam (prints the time, unix ms)
#   crash.sh restart            datadam again on its data; prints how long until it answered
#   crash.sh reboot             a hard reboot a second later: sysrq b, no sync, as a power cut would
#                               do to what is not on the disk yet (prints the time)
#   crash.sh rebooted           after the reboot: mounts /data again, re-arms the safety shutdown,
#                               starts datadam; exit 3 while the reboot has not happened yet
. /opt/datadam/cloud/node/common.sh

cmd=${1:?usage: crash.sh start | fsync <policy> | kill | restart | reboot | rebooted}
shift
ms() { date +%s%3N; }
state=/var/lib/datadam-crash
mkdir -p "$state"

# Until datadam answers (a long WAL replay can take minutes); prints the ms it took.
wait_up() {  # since (unix ms)
  python3 - "$PASSWORD" "$1" <<'EOF'
import socket, sys, time
password, since = sys.argv[1], int(sys.argv[2])
deadline = time.time() + 3600
while time.time() < deadline:
    try:
        with socket.create_connection(("127.0.0.1", 6379), timeout=2) as s:
            s.sendall(b"*2\r\n$4\r\nAUTH\r\n$%d\r\n%s\r\n*1\r\n$4\r\nPING\r\n" % (len(password), password.encode()))
            if b"PONG" in s.recv(1024) or b"PONG" in s.recv(1024):
                print(int(time.time() * 1000) - since)
                sys.exit(0)
    except OSError:
        pass
    time.sleep(0.1)
sys.exit("datadam did not answer within an hour")
EOF
}

case $cmd in
  start)
    /opt/datadam/cloud/node/server.sh start datadam
    ;;
  fsync)
    rcli 127.0.0.1 CONFIG SET fsync "${1:?policy}"
    ;;
  kill)
    echo "killed at $(ms)"
    docker kill -s KILL bench-server >/dev/null
    ;;
  restart)
    start=$(ms)
    docker start bench-server >/dev/null
    echo "answered $(wait_up "$start") ms after the restart ($(du -sm /data/datadam | cut -f1) MB of data)"
    ;;
  reboot)
    # What the next boot needs, on the root disk (not /data: syncing that would
    # defeat the power cut): the data device, the boot this is, and the time
    # of the safety shutdown, which a reboot cancels.
    # By filesystem UUID: NVMe device names change from one boot to the next.
    findmnt -no UUID /data > "$state/data-uuid"
    cat /proc/sys/kernel/random/boot_id > "$state/boot-id"
    sed -n 's/^USEC=//p' /run/systemd/shutdown/scheduled > "$state/shutdown-usec" 2>/dev/null || true
    ms > "$state/reboot-ms"
    sync -f "$state/data-uuid"
    echo 1 > /proc/sys/kernel/sysrq
    echo "reboot at $(cat "$state/reboot-ms")"
    nohup sh -c 'sleep 1; echo b > /proc/sysrq-trigger' >/dev/null 2>&1 &
    ;;
  rebooted)
    if [ "$(cat /proc/sys/kernel/random/boot_id)" = "$(cat "$state/boot-id")" ]; then
      echo "not rebooted yet" >&2
      exit 3
    fi
    booted=$(ms)
    uuid=$(cat "$state/data-uuid")
    findmnt /data >/dev/null || mount -o noatime UUID="$uuid" /data
    # Without /data, datadam would start empty and every key would read as lost.
    if [ "$(findmnt -no UUID /data)" != "$uuid" ]; then
      echo "/data is not the file system $uuid" >&2
      exit 1
    fi
    usec=$(cat "$state/shutdown-usec" 2>/dev/null)
    minutes=60
    [ -n "$usec" ] && minutes=$(( (usec / 1000000 - $(date +%s)) / 60 ))
    shutdown -h +"$(( minutes > 1 ? minutes : 1 ))" 2>/dev/null
    start=$(ms)
    docker start bench-server >/dev/null
    echo "up again $((booted - $(cat "$state/reboot-ms"))) ms after the reboot;" \
         "datadam answered $(wait_up "$start") ms after its start ($(du -sm /data/datadam | cut -f1) MB of data)"
    ;;
  *) echo "unknown command $cmd" >&2; exit 2 ;;
esac
