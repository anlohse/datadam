"""Scale results side by side: ops/s (memtier totals) and the server's median
%idle while loaded (mpstat rows below 90% idle), per node, store and workload.
Reps (-r1, -r2) are averaged.
  python bench/scale_compare.py <results dir> [<results dir> ...]"""
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

WORKLOADS = ["read-gaussian", "mixed-gaussian", "mixed-gaussian-p16"]


def idle(path):
    rows = []
    for line in path.read_text(errors="replace").splitlines():
        f = line.split()
        if len(f) >= 12 and f[1] == "all":
            try:
                v = float(f[-1])
            except ValueError:
                continue
            if v < 90:
                rows.append(v)
    return statistics.median(rows) if rows else None


def collect(root):
    ops, idl = defaultdict(list), defaultdict(list)
    for js in (root / "loader").glob("*.json"):
        name = js.stem
        wl = next((w for w in sorted(WORKLOADS, key=len, reverse=True) if name.endswith("-" + w)), None)
        if not wl:
            continue
        tag = name[: -len(wl) - 1]
        node = tag.split("-")[0]
        key = (node, re.sub(r"-r\d+$", "", tag[len(node) + 1:]), wl)
        try:
            ops[key].append(json.loads(js.read_text())["ALL STATS"]["Totals"]["Ops/sec"])
        except (KeyError, ValueError):
            continue
        mp = root / node / f"{name}-{node}-mpstat.txt"
        if mp.exists() and (v := idle(mp)) is not None:
            idl[key].append(v)
    return {k: (statistics.mean(v), statistics.mean(idl[k]) if idl[k] else None) for k, v in ops.items()}


def main():
    runs = {Path(d).name: collect(Path(d)) for d in sys.argv[1:]}
    keys = sorted({k for r in runs.values() for k in r},
                  key=lambda k: (int(k[0][1:]), k[1], WORKLOADS.index(k[2])))
    print(f"{'node':5} {'store':18} {'workload':20}" + "".join(f" {n[-15:]:>24}" for n in runs))
    for k in keys:
        cells = []
        for r in runs.values():
            if k in r:
                o, i = r[k]
                cells.append(f"{o / 1000:9.0f}k idle {i:4.0f}%" if i is not None else f"{o / 1000:9.0f}k" + " " * 11)
            else:
                cells.append(" " * 24)
        print(f"{k[0]:5} {k[1]:18} {k[2]:20}" + "".join(f" {c:>24}" for c in cells))


if __name__ == "__main__":
    main()
