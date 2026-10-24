#!/bin/sh
# Runs tests/integration_test.py against a fresh server, then checks that the
# data survives a kill -9 and that SIGTERM shuts the server down cleanly.
# usage: tests/run_integration.sh [path/to/datadam]
set -eu

BIN=${1:-build/linux/datadam}
PORT=${PORT:-6390}
MC_PORT=${MC_PORT:-11290}
DATA=$(mktemp -d)
PID=

cleanup() {
  [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null || true
  rm -rf "$DATA"
}
trap cleanup EXIT

start() {
  "$BIN" datadam.conf --bind 127.0.0.1 --port "$PORT" --memcache-port "$MC_PORT" --dir "$DATA" &
  PID=$!
  python3 - "$PORT" <<'EOF'
import socket, sys, time
for _ in range(100):
    try:
        socket.create_connection(("127.0.0.1", int(sys.argv[1]))).close()
        sys.exit(0)
    except OSError:
        time.sleep(0.1)
sys.exit("server did not start")
EOF
}

start
python3 tests/integration_test.py --port "$PORT" --memcache-port "$MC_PORT"

echo "== kill -9 and restart"
kill -9 "$PID"
wait "$PID" 2>/dev/null || true
start
python3 tests/integration_test.py --port "$PORT" --memcache-port "$MC_PORT" --phase verify

echo "== SIGTERM"
kill -TERM "$PID"
wait "$PID"
echo "server exited with status $?"
PID=
