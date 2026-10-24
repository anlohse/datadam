"""Tables from a bench/memtier.sh results directory.

usage: python bench/report.py bench/results/<scenario>-<time>
"""
import json
import pathlib
import sys

SERVERS = ["datadam", "kvrocks", "redis"]
WORKLOADS = ["read-uniform", "read-gaussian", "mixed-gaussian", "mixed-gaussian-p16", "write-uniform"]


def stats(path):
    d = json.loads(path.read_text())["ALL STATS"]
    t, g = d["Totals"], d["Gets"]
    p = t.get("Percentile Latencies", {})
    lookups = g.get("Hits/sec", 0) + g.get("Misses/sec", 0)
    return {
        "ops": t["Ops/sec"],
        "avg": t["Average Latency"],
        "p50": p.get("p50.00"),
        "p99": p.get("p99.00"),
        "p999": p.get("p99.90"),
        "hit": g["Hits/sec"] / lookups if lookups else None,
        "seconds": d["Runtime"]["Total duration"] / 1000.0,
    }


def fmt(v, digits=2):
    return "-" if v is None else f"{v:,.{digits}f}"


def main():
    out = pathlib.Path(sys.argv[1])
    notes = {}
    for line in (out / "notes.txt").read_text().splitlines() if (out / "notes.txt").exists() else []:
        server, key, value = (line.split(" ", 2) + [""])[:3]
        notes.setdefault(server, {})[key] = value
    servers = [s for s in SERVERS if any(out.glob(f"{s}-*.json"))]

    print(f"### {out.name}\n")
    print("| workload | " + " | ".join(f"{s} ops/s | p50 / p99 / p99.9 ms" for s in servers) + " |")
    print("|---|" + "---|---|" * len(servers))
    for w in WORKLOADS:
        cells = []
        for s in servers:
            f = out / f"{s}-{w}.json"
            if not f.exists():
                cells += ["-", "-"]
                continue
            r = stats(f)
            hit = f" (hits {r['hit']:.0%})" if w.startswith("read") and r["hit"] is not None else ""
            cells += [f"{r['ops']:,.0f}{hit}", f"{fmt(r['p50'])} / {fmt(r['p99'])} / {fmt(r['p999'])}"]
        print(f"| {w} | " + " | ".join(cells) + " |")

    print("\n| | " + " | ".join(servers) + " |")
    print("|---|" + "---|" * len(servers))
    rows = [
        ("load (random values)", lambda s: notes.get(s, {}).get("load_first", "-")),
        ("memory after load", lambda s: notes.get(s, {}).get("mem_after_load", "-")),
        ("disk after load (MB)", lambda s: notes.get(s, {}).get("disk_mb_after_load", "-")),
        ("memory at the end", lambda s: notes.get(s, {}).get("mem_end", "-")),
        ("disk at the end (MB)", lambda s: notes.get(s, {}).get("disk_mb_end", "-")),
        ("OOM killed", lambda s: notes.get(s, {}).get("oom_killed", "-")),
    ]
    for label, fn in rows:
        print(f"| {label} | " + " | ".join(fn(s) for s in servers) + " |")


if __name__ == "__main__":
    main()
