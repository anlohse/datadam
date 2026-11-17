#!/usr/bin/env bash
# redis-py's own test suite (sync and asyncio, standalone) against datadam.
# What datadam does not implement is deselected by tests/clients/redis-py.skip,
# one test per line with the reason; anything else that fails is a regression.
#   bash tests/clients/redis-py.sh [image]    (default ghcr.io/anlohse/datadam:edge)
# env: REDIS_PY_REF (the redis-py tag), PROTOCOL (2 or 3; default: the client's),
#      EXTRA_CA (a root CA for pip, e.g. an antivirus's), PYTEST_ARGS (more
#      arguments, e.g. a test file), OUT (a directory for the JUnit report),
#      KEEP=1 (leave the containers and network up)
set -euo pipefail
# Git Bash on Windows: container paths go to docker as they are (not to git).
dk() { MSYS_NO_PATHCONV=1 docker "$@"; }

IMAGE=${1:-ghcr.io/anlohse/datadam:edge}
REDIS_PY_REF=${REDIS_PY_REF:-v8.1.0}
HERE=$(cd "$(dirname "$0")" && pwd)
NET=dd-redis-py
WORK=$(mktemp -d)

cleanup() {
  [ -n "${KEEP:-}" ] && return
  dk rm -f dd-redis-py-server dd-redis-py-runner >/dev/null 2>&1 || true
  dk network rm $NET >/dev/null 2>&1 || true
  # The runner wrote as root: remove its files through a container.
  [ -d "$WORK/redis-py" ] && dk run --rm -v "$(host_path "$WORK")":/w python:3.12-slim rm -rf /w/redis-py >/dev/null 2>&1 || true
  rm -rf "$WORK" 2>/dev/null || true
}
trap cleanup EXIT
host_path() { (cd "$1" && (pwd -W 2>/dev/null || pwd)); }

git clone -q --depth 1 --branch "$REDIS_PY_REF" https://github.com/redis/redis-py.git "$WORK/redis-py" 2>/dev/null
# The skip list without its comments, one node id per line.
sed -e 's/[[:space:]]*#.*//' -e '/^[[:space:]]*$/d' "$HERE/redis-py.skip" > "$WORK/redis-py/deselect.txt"

dk network rm $NET >/dev/null 2>&1 || true
dk network create $NET >/dev/null
dk rm -f dd-redis-py-server >/dev/null 2>&1 || true
dk run -d --name dd-redis-py-server --network $NET "$IMAGE" --memcache-port 0 >/dev/null

ca=()
[ -n "${EXTRA_CA:-}" ] && ca=(-v "$EXTRA_CA:/extra-ca.crt:ro")
protocol=()
[ -n "${PROTOCOL:-}" ] && protocol=(--protocol="$PROTOCOL")
dk run --rm --name dd-redis-py-runner --network $NET "${ca[@]}" -v "$(host_path "$WORK/redis-py")":/redis-py -w /redis-py \
  python:3.12-slim bash -ec '
    if [ -f /extra-ca.crt ]; then
      cat "$(python -m pip debug 2>/dev/null | sed -n "s/.*cert: //p" | head -1)" /extra-ca.crt > /tmp/ca.pem 2>/dev/null ||
        cat /usr/local/lib/python3.12/site-packages/pip/_vendor/certifi/cacert.pem /extra-ca.crt > /tmp/ca.pem
      export PIP_CERT=/tmp/ca.pem
    fi
    pip install -q --root-user-action=ignore --disable-pip-version-check -e . -r dev_requirements.txt
    deselect=()
    while IFS= read -r id; do deselect+=(--deselect "$id"); done < deselect.txt
    # The selection redis-py runs against a standalone server, without modules;
    # Sentinel has its own suite here (tests/docker_sentinel_test.sh).
    exec pytest tests --redis-url=redis://dd-redis-py-server:6379/0 "$@" \
      --ignore=tests/test_scenario --ignore=tests/test_asyncio/test_scenario --ignore=tests/test_multidb \
      --ignore=tests/test_asyncio/test_multidb --ignore=tests/test_http --ignore=tests/maint_notifications \
      --ignore=tests/test_sentinel.py --ignore=tests/test_sentinel_managed_connection.py \
      --ignore=tests/test_asyncio/test_sentinel.py --ignore=tests/test_asyncio/test_sentinel_managed_connection.py \
      -m "not onlycluster and not redismod and not fixed_client and not multidb_integration and not ssl" \
      -p no:cacheprovider -o addopts= --timeout=60 -q -rfE --junit-xml=/redis-py/junit.xml "${deselect[@]}"
  ' redis-py "${protocol[@]}" ${PYTEST_ARGS:-} && status=0 || status=$?
if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp "$WORK/redis-py/junit.xml" "$OUT/redis-py-junit.xml" 2>/dev/null || true; fi
exit $status
