#!/usr/bin/env bash
# Runs the server image in Docker and tests it with the official redis-cli:
#   1. every supported command (tests/redis_cli_test.sh)
#   2. persistence across a graceful stop (SIGTERM) and restart
#   3. persistence across a crash (SIGKILL) and restart
#
# usage: tests/docker_redis_cli.sh [image]     (default: datadam:latest)
set -euo pipefail
export MSYS_NO_PATHCONV=1  # Git Bash on Windows: keep the volume paths as they are

IMAGE=${1:-datadam:latest}
NET=datadam-test-net
SRV=datadam-test-srv
VOL=datadam-test-data
TESTS_DIR=$(cd "$(dirname "$0")" && pwd -W 2>/dev/null || pwd)

cleanup() {
  docker rm -f "$SRV" >/dev/null 2>&1 || true
  docker volume rm "$VOL" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup
docker network create "$NET" >/dev/null
docker run -d --name "$SRV" --network "$NET" -v "$VOL":/var/lib/datadam "$IMAGE" --wal-retention 1h >/dev/null

cli_test() {
  docker run --rm --network "$NET" -v "$TESTS_DIR":/tests:ro redis:7.4 \
    bash /tests/redis_cli_test.sh "$SRV" 6379 "$1" | grep -vE '^  ok '
}

wait_ready() {
  for _ in $(seq 50); do
    docker run --rm --network "$NET" redis:7.4 redis-cli -h "$SRV" PING 2>/dev/null | grep -q PONG && return
    sleep 0.2
  done
  echo "server did not come up"; docker logs "$SRV"; exit 1
}

wait_ready
cli_test commands

for stop in "stop" "kill -s KILL"; do
  echo "### persistence: docker $stop"
  cli_test write
  docker $stop "$SRV" >/dev/null
  echo "server exit code: $(docker inspect -f '{{.State.ExitCode}}' "$SRV")"
  sleep 3  # long enough for the 1.5s-TTL key to expire while the server is down
  docker start "$SRV" >/dev/null
  wait_ready
  cli_test verify
done

# ---- the whole command suite again, over TLS with client certificates
echo "### TLS"
TLS_SRV=datadam-test-tls
TLS_DIR=$(mktemp -d)
cleanup_tls() { docker rm -f "$TLS_SRV" >/dev/null 2>&1 || true; docker volume rm "$VOL-tls" >/dev/null 2>&1 || true; rm -rf "$TLS_DIR"; }
trap 'cleanup_tls; cleanup' EXIT
(
  cd "$TLS_DIR"
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout ca.key -out ca.crt -days 1 \
    -subj /CN=datadam-test-ca 2>/dev/null
  printf 'subjectAltName=DNS:%s\nextendedKeyUsage=serverAuth,clientAuth\n' "$TLS_SRV" > ext.cnf
  for name in server client; do
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$name.key" -out "$name.csr" \
      -subj "/CN=$name" 2>/dev/null
    openssl x509 -req -in "$name.csr" -CA ca.crt -CAkey ca.key -CAcreateserial -out "$name.crt" -days 1 \
      -extfile ext.cnf 2>/dev/null
  done
  chmod 644 ./*.key
)
chmod 755 "$TLS_DIR"  # mktemp makes it 0700; the containers run as other users
TLS_MOUNT=$(cd "$TLS_DIR" && pwd -W 2>/dev/null || pwd)
docker run -d --name "$TLS_SRV" --network "$NET" -v "$VOL-tls":/var/lib/datadam -v "$TLS_MOUNT":/tls:ro "$IMAGE" \
  --tls-port 6380 --tls-cert-file /tls/server.crt --tls-key-file /tls/server.key --tls-ca-cert-file /tls/ca.crt \
  --tls-auth-clients-user cn >/dev/null
TLS_OPTS="--tls --cacert /tls/ca.crt --cert /tls/client.crt --key /tls/client.key"
tls_cli() { docker run --rm --network "$NET" -v "$TLS_MOUNT":/tls:ro redis:7.4 redis-cli -h "$TLS_SRV" "$@" 2>&1; }
for _ in $(seq 50); do tls_cli -p 6380 $TLS_OPTS PING | grep -q PONG && break; sleep 0.2; done
docker run --rm --network "$NET" -v "$TESTS_DIR":/tests:ro -v "$TLS_MOUNT":/tls:ro -e CLI_OPTS="$TLS_OPTS" redis:7.4 \
  bash /tests/redis_cli_test.sh "$TLS_SRV" 6380 commands | grep -vE '^  ok '
check() {  # name, expected substring, actual
  if [[ "$3" == *"$2"* ]]; then echo "  ok   $1"; else echo "  FAIL $1: expected *$2*, got: $3"; exit 1; fi
}
check "no client certificate: refused" "rror" "$(tls_cli -p 6380 --tls --cacert /tls/ca.crt PING)"
check "plain text on the TLS port: refused" "rror" "$(tls_cli -p 6380 PING)"
check "INFO counts TLS connections" "tls_connections:" "$(tls_cli -p 6380 $TLS_OPTS INFO stats)"
tls_cli -p 6380 $TLS_OPTS ACL SETUSER client on resetpass '~cert:*' +@all >/dev/null
tls_cli -p 6380 $TLS_OPTS CONFIG SET requirepass topsecret >/dev/null
check "certificate CN logs in as its user" "client" "$(tls_cli -p 6380 $TLS_OPTS ACL WHOAMI)"
check "with that user's permissions" "NOPERM" "$(tls_cli -p 6380 $TLS_OPTS GET other)"
check "plain port still needs the password" "NOAUTH" "$(tls_cli -p 6379 GET other)"
check "ACL LOG has the refusal" "other" "$(tls_cli -p 6379 --no-auth-warning -a topsecret ACL LOG)"

# ---- a replica, in its own container, of the plain server (which keeps its WAL)
echo "### replication"
REPL_SRV=datadam-test-replica
cleanup_repl() { docker rm -f "$REPL_SRV" >/dev/null 2>&1 || true; docker volume rm "$VOL-repl" >/dev/null 2>&1 || true; }
trap 'cleanup_repl; cleanup_tls; cleanup' EXIT
pcli() { docker run --rm --network "$NET" redis:7.4 redis-cli -h "$SRV" "$@" 2>&1; }
rcli() { docker run --rm --network "$NET" redis:7.4 redis-cli -h "$REPL_SRV" "$@" 2>&1; }
pcli SET repl:before v1 >/dev/null
pcli HSET repl:h f v >/dev/null
docker run -d --name "$REPL_SRV" --network "$NET" -v "$VOL-repl":/var/lib/datadam "$IMAGE" \
  --replicaof "$SRV 6379" >/dev/null
synced() {
  for _ in $(seq 100); do
    [ "$(rcli GET "$1")" = "$2" ] && return 0
    sleep 0.2
  done
  return 1
}
synced repl:before v1 && echo "  ok   full sync" || { echo "  FAIL full sync"; docker logs "$REPL_SRV"; exit 1; }
check "hash arrived" "v" "$(rcli HGET repl:h f)"
pcli SET repl:after v2 >/dev/null
synced repl:after v2 && echo "  ok   streamed write" || { echo "  FAIL streamed write"; exit 1; }
check "replica refuses writes" "READONLY" "$(rcli SET repl:x y)"
check "ROLE on the replica" "slave" "$(rcli ROLE)"
check "INFO on the primary" "connected_slaves:1" "$(pcli INFO replication)"
check "WAIT for the replica" "1" "$(pcli WAIT 1 2000)"
check "WAIT on a replica" "cannot be used with replica" "$(rcli WAIT 1 0)"
check "FAILOVER to a stranger" "is not a replica" "$(pcli FAILOVER TO 10.9.9.9 1)"
check "promoted" "OK" "$(rcli REPLICAOF NO ONE)"
check "writes after promotion" "OK" "$(rcli SET repl:x y)"

# ---- protected mode (the image turns it off; here it is on, as in datadam.conf)
echo "### protected mode"
PM_SRV=datadam-test-protected
cleanup_pm() { docker rm -f "$PM_SRV" >/dev/null 2>&1 || true; }
trap 'cleanup_pm; cleanup_repl; cleanup_tls; cleanup' EXIT
docker run -d --name "$PM_SRV" --network "$NET" "$IMAGE" --protected-mode yes >/dev/null
mcli() { docker run --rm --network "$NET" redis:7.4 redis-cli -h "$PM_SRV" "$@" 2>&1; }
for _ in $(seq 50); do mcli PING | grep -q "DENIED" && break; sleep 0.2; done
check "remote client refused" "DENIED datadam is running in protected mode" "$(mcli PING)"
check "memcache refused too" "SERVER_ERROR datadam is running in protected mode" \
  "$(docker run --rm --network "$NET" redis:7.4 bash -c "exec 3<>/dev/tcp/$PM_SRV/11211; printf 'version\r\n' >&3; head -c 60 <&3")"
docker rm -f "$PM_SRV" >/dev/null
docker run -d --name "$PM_SRV" --network "$NET" "$IMAGE" --protected-mode yes --requirepass s3cret >/dev/null
for _ in $(seq 50); do mcli -a s3cret --no-auth-warning PING 2>/dev/null | grep -q PONG && break; sleep 0.2; done
check "with a password, remote clients log in" "PONG" "$(mcli -a s3cret --no-auth-warning PING)"
echo "all redis-cli tests passed"
