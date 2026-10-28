"""Summary of a soak run (cloud phase 7): what grew, what drifted.

  python soak_report.py <results dir> [bin minutes, default 60]

Reads <dir>/server/soak/metrics.txt (datadam's /metrics, sampled), disk.txt
("<unix ms> <bytes of the data directory> <load averages>") and
<dir>/loader/soak/chunk-*-{main,ttl}.json (memtier, one per chunk). Prints one
row per bin (memory, disk, compaction debt, stalls, throughput, latency), the
change per hour of each over the run (a least-squares slope), and writes the
series to <dir>/soak-report.json.
"""
import glob
import json
import os
import sys

# Series from /metrics: name in the report -> metric (gauges as they are, counters as a rate per second).
GAUGES = {
    "rss_gb": ("datadam_used_memory_rss", 1e-9),
    "used_gb": ("datadam_used_memory", 1e-9),
    "cache_gb": ("datadam_cache_used_memory", 1e-9),
    "cache_items_m": ("datadam_cache_items", 1e-6),
    "sst_gb": ("datadam_rocksdb_live_sst_bytes", 1e-9),
    "memtable_gb": ("datadam_rocksdb_memtable_bytes", 1e-9),
    "wal_gb": ("datadam_rocksdb_live_wal_bytes", 1e-9),
    "block_cache_gb": ("datadam_rocksdb_block_cache_usage", 1e-9),
    "pending_compaction_gb": ("datadam_rocksdb_pending_compaction_bytes", 1e-9),
    "keys_m": ("datadam_rocksdb_estimate_num_keys", 1e-6),
    "clients": ("datadam_connected_clients", 1),
}
COUNTERS = {
    "stall_delays": "datadam_rocksdb_write_stall_delays",
    "stall_stops": "datadam_rocksdb_write_stall_stops",
    "commands": "datadam_commands_processed",
}


def parse_metrics(path):
    samples = []
    current = None
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("# t "):
                current = {"t": int(line[4:])}
                samples.append(current)
            elif current is not None and line and not line.startswith("#"):
                name, _, value = line.rstrip("\n").rpartition(" ")
                if "{" not in name:
                    try:
                        current[name] = float(value)
                    except ValueError:
                        pass
    return [s for s in samples if len(s) > 1]


def metric(sample, name):
    """A counter may be exported as name_total."""
    if name in sample:
        return sample[name]
    return sample.get(name + "_total")


def chunks(loader_dir, kind):
    out = []
    for path in sorted(glob.glob(os.path.join(loader_dir, f"chunk-*-{kind}.json"))):
        try:
            with open(path) as f:
                a = json.load(f)["ALL STATS"]
        except (OSError, ValueError, KeyError):
            continue
        row = {"t": a["Runtime"]["Start time"], "seconds": a["Runtime"]["Total duration"] / 1000,
               "ops_s": a["Totals"]["Ops/sec"]}
        for op in ("Gets", "Sets"):
            p = a.get(op, {}).get("Percentile Latencies", {})
            for q, short in (("p50.00", "p50"), ("p99.00", "p99"), ("p99.90", "p999")):
                if q in p:
                    row[f"{op.lower()}_{short}"] = p[q]
        out.append(row)
    return out


def slope_per_hour(points):
    """Least-squares slope of (unix ms, value) points, per hour."""
    if len(points) < 2:
        return None
    xs = [t / 3_600_000 for t, _ in points]
    ys = [v for _, v in points]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    bin_ms = int(float(sys.argv[2]) * 60_000) if len(sys.argv) > 2 else 3_600_000
    samples = parse_metrics(os.path.join(root, "server", "soak", "metrics.txt"))
    disk = []
    disk_path = os.path.join(root, "server", "soak", "disk.txt")
    if os.path.exists(disk_path):
        with open(disk_path) as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2 and parts[1].isdigit():
                    disk.append((int(parts[0]), int(parts[1]) / 1e9))
    main_chunks = chunks(os.path.join(root, "loader", "soak"), "main")
    ttl_chunks = chunks(os.path.join(root, "loader", "soak"), "ttl")
    # The soak starts with the first chunk: before it is the load.
    start = main_chunks[0]["t"] if main_chunks else (samples[0]["t"] if samples else 0)

    series = {name: [(s["t"], metric(s, m) * k) for s in samples if metric(s, m) is not None]
              for name, (m, k) in GAUGES.items()}
    for name, m in COUNTERS.items():
        pts = [(s["t"], metric(s, m)) for s in samples if metric(s, m) is not None]
        series[name + "_per_s"] = [(t1, (v1 - v0) / ((t1 - t0) / 1000)) for (t0, v0), (t1, v1) in zip(pts, pts[1:])
                                   if t1 > t0 and v1 >= v0]
    series["disk_gb"] = disk
    for kind, rows in (("main", main_chunks), ("ttl", ttl_chunks)):
        for key in sorted({k for r in rows for k in r} - {"t", "seconds"}):
            series[f"{kind}_{key}"] = [(r["t"], r[key]) for r in rows if key in r]

    soak = {name: [(t, v) for t, v in pts if t >= start] for name, pts in series.items()}
    columns = ["rss_gb", "cache_gb", "sst_gb", "wal_gb", "disk_gb", "pending_compaction_gb", "stall_delays_per_s",
               "stall_stops_per_s", "main_ops_s", "main_gets_p50", "main_gets_p99", "main_gets_p999",
               "main_sets_p99", "ttl_sets_p99"]
    end = max((t for pts in soak.values() for t, _ in pts), default=start)
    print("hour  " + " ".join(f"{c:>12}" for c in columns))
    b = start
    while b <= end:
        row = []
        for c in columns:
            vals = [v for t, v in soak.get(c, []) if b <= t < b + bin_ms]
            row.append(f"{sum(vals) / len(vals):12.3f}" if vals else f"{'-':>12}")
        print(f"{(b - start) / 3_600_000:4.1f}  " + " ".join(row))
        b += bin_ms
    slopes = {name: slope_per_hour(pts) for name, pts in soak.items()}
    print("\nchange per hour over the soak (least squares):")
    for name in columns + ["used_gb", "memtable_gb", "block_cache_gb", "cache_items_m", "keys_m"]:
        if slopes.get(name) is not None:
            print(f"  {name:24} {slopes[name]:+.4f}")
    with open(os.path.join(root, "soak-report.json"), "w") as f:
        json.dump({"start": start, "bin_ms": bin_ms, "slopes_per_hour": slopes, "series": series}, f)


if __name__ == "__main__":
    main()
