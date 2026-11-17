#!/usr/bin/env bash
# node-redlock's own test suite against datadam: one server, then three
# independent servers (the Redlock algorithm proper). Its cluster runs are
# left out: datadam has no cluster mode.
#   bash tests/clients/redlock.sh [image]     (default ghcr.io/anlohse/datadam:edge)
# env: REDLOCK_REF (the node-redlock tag), EXTRA_CA (a root CA for npm, e.g. an
#      antivirus's), KEEP=1 (leave the containers and network up)
set -euo pipefail
# Git Bash on Windows: container paths go to docker as they are (not to git).
dk() { MSYS_NO_PATHCONV=1 docker "$@"; }

IMAGE=${1:-ghcr.io/anlohse/datadam:edge}
REDLOCK_REF=${REDLOCK_REF:-v5.0.0-beta.2}   # what npm installs as redlock@5.0.0-beta.2
NET=dd-redlock
NODES=(redis-single-instance redis-multi-instance-a redis-multi-instance-b redis-multi-instance-c)
WORK=$(mktemp -d)

cleanup() {
  [ -n "${KEEP:-}" ] && return
  dk rm -f "${NODES[@]}" dd-redlock-runner >/dev/null 2>&1 || true
  dk network rm $NET >/dev/null 2>&1 || true
  # The runner wrote as root: remove its files through a container.
  [ -d "$WORK/node-redlock" ] && dk run --rm -v "$(host_path "$WORK")":/w node:20 rm -rf /w/node-redlock >/dev/null 2>&1 || true
  rm -rf "$WORK" 2>/dev/null || true
}
trap cleanup EXIT
host_path() { (cd "$1" && (pwd -W 2>/dev/null || pwd)); }

git clone -q --depth 1 --branch "$REDLOCK_REF" https://github.com/mike-marcacci/node-redlock.git "$WORK/node-redlock"
# The cluster runs connect to hosts that do not exist here: drop them.
perl -0pi -e 's/\nrun\(\s*"cluster",.*?\);[ \t\r]*(\n|\z)/\n/s' "$WORK/node-redlock/src/single.test.ts" "$WORK/node-redlock/src/multi.test.ts"
if grep -q '"cluster",' "$WORK/node-redlock/src/"*.test.ts; then
  echo "redlock: could not remove the cluster runs" >&2
  exit 1
fi

dk network rm $NET >/dev/null 2>&1 || true
dk network create $NET >/dev/null
for n in "${NODES[@]}"; do
  dk rm -f "$n" >/dev/null 2>&1 || true
  dk run -d --name "$n" --network $NET "$IMAGE" --memcache-port 0 >/dev/null
done

ca=()
[ -n "${EXTRA_CA:-}" ] && ca=(-v "$EXTRA_CA:/extra-ca.crt:ro" -e NODE_EXTRA_CA_CERTS=/extra-ca.crt)
dk run --rm --name dd-redlock-runner --network $NET "${ca[@]}" -v "$(host_path "$WORK/node-redlock")":/work -w /work \
  node:20 sh -ec '
    npm install --no-audit --no-fund --loglevel=error >/dev/null
    npx tsc
    cd dist/esm && npx ava --verbose --timeout=60s "*.test.js"'
