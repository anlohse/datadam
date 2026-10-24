# shellcheck shell=bash
# Sourced by the cloud/node scripts on a test node (as root, through SSM).
set -euo pipefail
# shellcheck disable=SC1091
. /etc/datadam-bench.env

RESULTS=/data/results/$RUN_ID
mkdir -p "$RESULTS"

# The private address of a node, or of the first node with a role.
node_ip() {
  local e n r ip
  for e in $NODES; do
    IFS='=' read -r n r ip <<< "$e"
    [ "$n" = "$1" ] && { echo "$ip"; return; }
  done
  return 1
}
first_of_role() {
  local e n r ip
  for e in $NODES; do
    IFS='=' read -r n r ip <<< "$e"
    [ "$r" = "$1" ] && { echo "$ip"; return; }
  done
  return 1
}

# redis-cli against a host, logged in.
rcli() {
  local host=$1
  shift
  docker run --rm --network host redis:7.4 redis-cli -h "$host" -a "$PASSWORD" --no-auth-warning "$@"
}

wait_ping() {  # host [port]
  for _ in $(seq 150); do
    docker run --rm --network host redis:7.4 redis-cli -h "$1" -p "${2:-6379}" -a "$PASSWORD" --no-auth-warning PING \
      2>/dev/null | grep -q PONG && return 0
    sleep 0.2
  done
  echo "no PONG from $1" >&2
  return 1
}

# Copies this node's results to the run's bucket, under <run>/<node>/.
upload() {
  aws s3 cp --recursive --only-show-errors --region "$REGION" "$RESULTS" "s3://$BUCKET/$RUN_ID/$NODE/"
}
