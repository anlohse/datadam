#!/usr/bin/env bash
# Runs every fuzz target for a while, in the image of the Dockerfile's fuzz stage.
#   bash fuzz/run.sh [seconds per target, default 60] [image, default datadam:fuzz]
# Crash inputs go to ./fuzz-artifacts/; it exits 1 if any target found something.
set -uo pipefail
export MSYS_NO_PATHCONV=1
SECONDS_PER=${1:-60}
IMAGE=${2:-datadam:fuzz}
mkdir -p fuzz-artifacts
OUT=$(cd fuzz-artifacts && (pwd -W 2>/dev/null || pwd))
failed=0
for t in resp json records commands; do
  extra=()
  # The command target keeps a database (with RocksDB threads) across inputs:
  # leak checks between inputs report its live state, and LeakSanitizer's
  # thread tracer crashes in containers. Leaks are covered by the ASan tests.
  [ "$t" = commands ] && extra=(-dict=fuzz/commands.dict -detect_leaks=0)
  if docker run --rm -v "$OUT":/out -e ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-18 "$IMAGE" \
      "build/linux-fuzz/fuzz_$t" "${extra[@]}" -max_total_time="$SECONDS_PER" -timeout=10 -rss_limit_mb=4096 \
      -artifact_prefix="/out/$t-" -print_final_stats=1 "fuzz/corpus/$t" > "fuzz-artifacts/$t.log" 2>&1; then
    echo "ok   $t: $(grep -aoE 'Done [0-9]+ runs' "fuzz-artifacts/$t.log")"
  else
    echo "FAIL $t:"
    grep -aE 'ERROR|SUMMARY|runtime error|^ +#[0-9]+ .* in datadam' "fuzz-artifacts/$t.log" | head -20
    failed=1
  fi
done
exit $failed
