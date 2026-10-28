#!/usr/bin/env python3
"""Runs the cloud test from this machine: commands on the nodes (through SSM),
results back from the bucket, and a check that nothing is left after
`terraform destroy`. Needs the AWS CLI v2 (logged in) and Terraform.

  python cloud/cloud.py wait                     every node up and set up
  python cloud/cloud.py run <node> <command...>  run a shell command on a node, print its output
  python cloud/cloud.py smoke                    phase 1: start datadam, run cloud/node/smoke.sh on the loader
  python cloud/cloud.py memory [--pilot]         phase 2: in memory, datadam x Redis x Kvrocks
  python cloud/cloud.py large [--pilot]          phase 3: larger than memory, datadam x Kvrocks
  python cloud/cloud.py scale [--pilot]          phase 4: 4/8/16 vCPUs, datadam x Redis x Kvrocks, io-threads sweep
  python cloud/cloud.py scale-image <image>      phase 4 again, datadam only, from another image
  python cloud/cloud.py ha [--pilot]             phase 5: kill, restart, network cut, planned failover
  python cloud/cloud.py crash [--pilot]          phase 6: kill -9 and hard reboots under writes, every key checked
  python cloud/cloud.py collect                  results of the run -> bench/results/cloud-<run>-<time>/
  python cloud/cloud.py leftovers [region]       anything of datadam-bench still in the account?
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
TF_DIR = os.path.join(HERE, "terraform")
REPO = os.path.dirname(HERE)


def sh(args, capture=True, check=True):
    r = subprocess.run(args, capture_output=capture, text=True)
    if check and r.returncode != 0:
        sys.exit(f"command failed ({r.returncode}): {' '.join(args)}\n{r.stderr if capture else ''}")
    return r.stdout if capture else ""


def aws(*args, region=None):
    cmd = ["aws", *args, "--output", "json"]
    if region:
        cmd += ["--region", region]
    out = sh(cmd)
    return json.loads(out) if out.strip() else {}


def tf_outputs():
    out = json.loads(sh(["terraform", f"-chdir={TF_DIR}", "output", "-json"]))
    if not out:
        sys.exit("no Terraform outputs: run `terraform apply` in cloud/terraform first")
    return {k: v["value"] for k, v in out.items()}


def run_on(outs, node, command, timeout_s=3600, quiet=False):
    """Runs a shell command on a node as root; returns (exit code, stdout, stderr)."""
    nodes = outs["nodes"]
    if node not in nodes:
        sys.exit(f"no node {node}; nodes: {', '.join(nodes)}")
    params = {"commands": [command], "executionTimeout": [str(timeout_s)]}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump(params, f)
        path = f.name
    try:
        sent = aws("ssm", "send-command", "--instance-ids", nodes[node]["id"],
                   "--document-name", "AWS-RunShellScript", "--parameters", f"file://{path}",
                   "--timeout-seconds", "600", "--comment", f"datadam-bench {node}",
                   "--output-s3-bucket-name", outs["bucket"], "--output-s3-key-prefix", "ssm",
                   region=outs["region"])
    finally:
        os.unlink(path)
    cmd_id = sent["Command"]["CommandId"]
    if not quiet:
        print(f"[{node}] {command}", flush=True)
    while True:
        time.sleep(3)
        r = subprocess.run(["aws", "ssm", "get-command-invocation", "--command-id", cmd_id, "--instance-id",
                            nodes[node]["id"], "--region", outs["region"], "--output", "json"],
                           capture_output=True, text=True)
        if r.returncode != 0:  # not registered yet
            continue
        inv = json.loads(r.stdout)
        if inv["Status"] in ("Pending", "InProgress", "Delayed"):
            continue
        out, err = inv.get("StandardOutputContent", ""), inv.get("StandardErrorContent", "")
        if not quiet:
            if out:
                print(out, end="" if out.endswith("\n") else "\n")
            if err:
                print(err, end="" if err.endswith("\n") else "\n", file=sys.stderr)
            if inv["Status"] != "Success":
                print(f"[{node}] {inv['Status']} (exit {inv.get('ResponseCode')})", file=sys.stderr)
        return inv.get("ResponseCode", -1), out, err


def wait_ready(outs, limit_s=900):
    """Every node registered with SSM and done with cloud-init."""
    ids = {n: v["id"] for n, v in outs["nodes"].items()}
    deadline = time.time() + limit_s
    pending = set(ids)
    while pending and time.time() < deadline:
        info = aws("ssm", "describe-instance-information", "--filters",
                   f"Key=InstanceIds,Values={','.join(ids[n] for n in pending)}", region=outs["region"])
        online = {i["InstanceId"] for i in info.get("InstanceInformationList", []) if i["PingStatus"] == "Online"}
        for n in sorted(pending):
            if ids[n] in online:
                code, _, _ = run_on(outs, n, "test -f /run/datadam-ready", timeout_s=60, quiet=True)
                if code == 0:
                    print(f"{n}: ready", flush=True)
                    pending.discard(n)
        if pending:
            time.sleep(10)
    if pending:
        sys.exit(f"not ready after {limit_s} s: {', '.join(sorted(pending))} "
                 "(see /var/log/datadam-setup.log on the node)")


# ---- phase 2: in memory

NODE = "/opt/datadam/cloud/node"
# 200 connections from the loader; values of 512 incompressible bytes for the
# load, memtier's own for its writes.
MEMTIER = "--threads=8 --clients=25 --data-size=512 --random-data --distinct-client-seed"
WORKLOADS = [  # name, memtier arguments, writes?
    ("read-uniform", "--ratio=0:1 --key-pattern=R:R", False),
    ("read-gaussian", "--ratio=0:1 --key-pattern=G:G", False),
    ("mixed-gaussian", "--ratio=1:4 --key-pattern=G:G", True),
    ("mixed-gaussian-p16", "--ratio=1:4 --key-pattern=G:G --pipeline=16", True),
    ("write-uniform", "--ratio=1:0 --key-pattern=R:R", True),
]
# The latency curve: the mixed workload at fixed offered loads (requests per
# second in total), the same for every server, instead of as fast as it goes.
RATES = [100_000, 250_000, 400_000]


def scenario_memory(outs, reps, seconds, keys, servers, rates):
    """Each server in turn, `reps` times (rotating which goes first): the five
    workloads of bench/memtier.sh, then the latency curve. A workload that
    writes starts from a freshly loaded data set (memtier's values compress)."""
    wait_ready(outs)

    def step(node, command):
        code, _, _ = run_on(outs, node, command)
        if code != 0:
            sys.exit(f"stopped: {command} failed on {node}")

    def fresh(server, tag):
        step("server", f"{NODE}/server.sh start {server}")
        step("loader", f"{NODE}/dataset.sh {tag} {keys}")

    def measure(tag, name, args):
        step("server", f"{NODE}/sysstats.sh start {tag}-{name}")
        step("loader", f"{NODE}/load.sh {tag}-{name} {MEMTIER} --key-maximum={keys} --test-time={seconds} {args}")
        step("server", f"{NODE}/sysstats.sh stop")

    order = list(servers)
    for rep in range(1, reps + 1):
        for server in order:
            tag = f"{server}-r{rep}"
            fresh(server, tag)
            for name, args, writes in WORKLOADS:
                if writes:
                    fresh(server, f"{tag}-{name}")
                measure(tag, name, args)
            fresh(server, f"{tag}-curve")
            for rate in rates:
                per_connection = max(1, rate // 200)
                measure(tag, f"rate{rate // 1000}k", f"--ratio=1:4 --key-pattern=G:G --rate-limiting={per_connection}")
        order = order[1:] + order[:1]
    step("server", f"{NODE}/server.sh stop")
    collect(outs)


# ---- phase 3: larger than memory

# Reads first: the writes change the data set, which is loaded once per server
# (hundreds of GB take most of an hour). Longer runs: a cold cache warms up
# during the first seconds of each (memtier's JSON has the per-second series).
LARGE_WORKLOADS = [  # name, memtier arguments, seconds x
    ("read-uniform", "--ratio=0:1 --key-pattern=R:R", 2),
    ("read-gaussian", "--ratio=0:1 --key-pattern=G:G", 2),
    ("mixed-gaussian", "--ratio=1:4 --key-pattern=G:G", 2),
    ("mixed-gaussian-p16", "--ratio=1:4 --key-pattern=G:G --pipeline=16", 1),
    ("write-uniform", "--ratio=1:0 --key-pattern=R:R", 2),
]
# --randomize: the per-client seeds are otherwise the same in every run, and a
# run would replay the keys the one before it just read (on 2026-10-27 the
# first 36 s of the mixed run came from datadam's cache, without the disk).
LARGE_MEMTIER = "--threads=8 --clients=25 --data-size=1000 --random-data --distinct-client-seed --randomize"
# Offered loads of the latency curve, below what the disk-bound runs reach.
LARGE_RATES = [25_000, 50_000, 100_000]


def scenario_large(outs, keys, value_bytes, seconds, settle_s, reps, servers, rates):
    """Each server in turn: load `keys` incompressible values (timed, with the
    machine's statistics), leave it `settle_s` seconds to compact, then the
    workloads `reps` times and the latency curve, with the size on disk
    before and after."""
    wait_ready(outs)

    def step(node, command, timeout_s=3600):
        code, _, _ = run_on(outs, node, command, timeout_s=timeout_s)
        if code != 0:
            sys.exit(f"stopped: {command} failed on {node}")

    def measure(tag, name, args, secs):
        step("server", f"{NODE}/sysstats.sh start {tag}-{name}")
        step("loader", f"{NODE}/load.sh {tag}-{name} {LARGE_MEMTIER} --key-maximum={keys} --test-time={secs} {args}")
        step("server", f"{NODE}/sysstats.sh stop")

    def size(tag):
        step("server", f". /etc/datadam-bench.env; {NODE}/server.sh du | tee /data/results/$RUN_ID/{tag}-du.txt; "
                       f"{NODE}/server.sh info persistence > /data/results/$RUN_ID/{tag}-persistence.txt 2>&1 || true")

    for server in servers:
        step("server", f"{NODE}/server.sh start {server}")
        step("server", f"{NODE}/sysstats.sh start {server}-load")
        step("loader", f"{NODE}/dataset.sh {server} {keys} {value_bytes} 12", timeout_s=6 * 3600)
        size(f"{server}-loaded")
        if settle_s:
            step("server", f"sleep {settle_s}", timeout_s=settle_s + 600)
        step("server", f"{NODE}/sysstats.sh stop")
        size(f"{server}-settled")
        for rep in range(1, reps + 1):
            for name, args, x in LARGE_WORKLOADS:
                measure(f"{server}-r{rep}", name, args, seconds * x)
        for rate in rates:
            per_connection = max(1, rate // 200)
            measure(server, f"rate{rate // 1000}k", f"--ratio=1:4 --key-pattern=G:G --rate-limiting={per_connection}",
                    seconds)
        size(f"{server}-end")
        step("server", f"{NODE}/sysstats.sh stop")  # uploads the size files too
    step("server", f"{NODE}/server.sh stop")
    collect(outs)


# ---- phase 4: scaling with cores

# Three servers of the same family, 4, 8 and 16 vCPUs, measured one at a time
# from one loader. 800 connections: with 200, requests without a pipeline are
# bounded by the round trip (phase 2), not by the server's cores.
SCALE_SERVERS = [("s4", 4), ("s8", 8), ("s16", 16)]
SCALE_MEMTIER = "--threads=16 --clients=50 --data-size=512 --random-data --distinct-client-seed --randomize"
SCALE_WORKLOADS = [  # name, memtier arguments
    ("read-gaussian", "--ratio=0:1 --key-pattern=G:G"),
    ("mixed-gaussian", "--ratio=1:4 --key-pattern=G:G"),
    ("mixed-gaussian-p16", "--ratio=1:4 --key-pattern=G:G --pipeline=16"),
]
IO_THREADS = [1, 2, 4, 8, 16]


def scenario_scale(outs, reps, seconds, keys, stores, sweep, image=None):
    """For each size, every store in turn (rotating which goes first), `reps`
    times; then datadam with each `io-threads` up to the size's vCPUs. With
    `image`, the store "datadam-new" is datadam from that image."""
    wait_ready(outs)

    def step(node, command):
        code, _, _ = run_on(outs, node, command)
        if code != 0:
            sys.exit(f"stopped: {command} failed on {node}")

    def fresh(node, store, tag, args=""):
        if store == "datadam-new":
            step(node, f"DATADAM_IMAGE={image} {NODE}/server.sh start datadam {args}")
        else:
            step(node, f"{NODE}/server.sh start {store} {args}")
        step("loader", f"TARGET={node} {NODE}/dataset.sh {tag} {keys}")

    def measure(node, tag, name, args):
        step(node, f"{NODE}/sysstats.sh start {tag}-{name}")
        step("loader", f"TARGET={node} {NODE}/load.sh {tag}-{name} {SCALE_MEMTIER} --key-maximum={keys} "
                       f"--test-time={seconds} {args}")
        step(node, f"{NODE}/sysstats.sh stop")

    order = list(stores)
    for rep in range(1, reps + 1):
        for node, _ in SCALE_SERVERS:
            for store in order:
                tag = f"{node}-{store}-r{rep}"
                fresh(node, store, tag)
                for name, args in SCALE_WORKLOADS:
                    measure(node, tag, name, args)
            step(node, f"{NODE}/server.sh stop")
        order = order[1:] + order[:1]
    for node, cpus in SCALE_SERVERS:
        for threads in [t for t in sweep if t <= cpus]:
            tag = f"{node}-{stores[0]}-io{threads}"
            fresh(node, stores[0], tag, f"--io-threads {threads}")
            for name, args in SCALE_WORKLOADS[1:]:
                measure(node, tag, name, args)
        step(node, f"{NODE}/server.sh stop")
    collect(outs)


# ---- phase 5: high availability

def scenario_ha(outs, keys, value_bytes, write_s, fault_at_s, cut_s, workers):
    """A primary (the server node), the replicas and three sentinels; the
    loader writes with WAIT 1 (bench/ha_check.py) while, in turn:
      kill      the primary is killed (kill -9); it comes back empty, as a
                replica: a full sync of the whole data set;
      restart   a replica restarts, keeping its data: it catches up;
      cut       the primary loses the replicas and the sentinels for cut_s
                (the loader still reaches it), then the network heals;
      failover  a planned SENTINEL FAILOVER.
    After each, every acknowledged write is looked for on every data node."""
    wait_ready(outs)
    nodes = outs["nodes"]
    by_ip = {v["ip"]: n for n, v in nodes.items()}
    replicas = sorted(n for n, v in nodes.items() if v["role"] == "replica")
    others = lambda p: [v["ip"] for n, v in nodes.items() if n != p and v["role"] in ("server", "replica", "sentinel")]

    def step(node, command):
        code, out, _ = run_on(outs, node, command)
        if code != 0:
            sys.exit(f"stopped: {command} failed on {node}")
        return out

    def primary():
        ip = step("loader", f"{NODE}/ha_load.sh primary").strip()
        if ip not in by_ip:
            sys.exit(f"the sentinels name no known primary: {ip!r}")
        return by_ip[ip]

    def under_writes(name, seconds, fault):
        """The writes run for `seconds`; fault() starts fault_at_s after them."""
        writer = threading.Thread(target=step, args=("loader", f"{NODE}/ha_load.sh write {name} {seconds} {workers}"))
        writer.start()
        time.sleep(fault_at_s)
        fault()
        writer.join()
        step("loader", f"{NODE}/ha_load.sh verify {name}")

    # Sentinels from a run before would re-point the fresh primary (and kill its
    # clients doing so): they start again once the data nodes are set up.
    sentinels = sorted(n for n, v in nodes.items() if v["role"] == "sentinel")
    for s in sentinels:
        step(s, "docker rm -f bench-sentinel >/dev/null 2>&1 || true")
    step("server", f"{NODE}/ha.sh primary")
    for r in replicas:
        step(r, f"{NODE}/ha.sh rejoin {nodes['server']['ip']}")
    for s in sentinels:
        step(s, f"{NODE}/sentinel.sh")
    step("loader", f"{NODE}/ha_load.sh ready {len(replicas)}")
    step("loader", f"TARGET=server {NODE}/dataset.sh ha-preload {keys} {value_bytes}")
    for r in replicas:
        step(r, f"{NODE}/ha.sh synced {nodes['server']['ip']}")

    p = primary()
    under_writes("kill", write_s, lambda: step(p, f"{NODE}/ha.sh kill"))
    step(p, f"{NODE}/ha.sh rejoin {nodes[primary()]['ip']}")

    p = primary()
    r = next(n for n in replicas + ["server"] if n != p)
    under_writes("restart", write_s, lambda: step(r, f"{NODE}/ha.sh restart"))
    step(r, f"{NODE}/ha.sh synced {nodes[p]['ip']}")

    p = primary()

    def cut():
        step(p, f"{NODE}/ha.sh cut {' '.join(others(p))}")
        time.sleep(cut_s)
        step(p, f"{NODE}/ha.sh heal")

    under_writes("cut", write_s + cut_s, cut)
    step(p, f"{NODE}/ha.sh synced {nodes[primary()]['ip']}")

    p = primary()
    under_writes("failover", write_s, lambda: step("loader", f"{NODE}/ha_load.sh failover"))
    step(p, f"{NODE}/ha.sh synced {nodes[primary()]['ip']}")
    collect(outs)


# ---- phase 6: crash

def scenario_crash(outs, keys, value_bytes, write_s, fault_at_s, workers, pipeline, churn, cases):
    """datadam on the server, `keys` of `value_bytes` loaded; then for each case
    (kill or reboot, with an fsync policy) the loader writes numbered operations
    (bench/crash_check.py) while the server is killed (kill -9) or hard-rebooted
    (sysrq b: no sync, as a power cut), started again, and every acknowledged
    write looked for. A kill must lose nothing whatever the fsync; a reboot
    nothing with fsync always, at most about the last second with everysec.
    At the end every case is checked again: later crashes must not lose what
    earlier ones kept."""
    wait_ready(outs)

    def step(node, command):
        code, out, _ = run_on(outs, node, command)
        if code != 0:
            sys.exit(f"stopped: {command} failed on {node}")
        return out

    def rebooted():
        time.sleep(15)
        for _ in range(60):  # until the node is back and has rebooted (exit 3: not yet)
            code, _, _ = run_on(outs, "server", f"{NODE}/crash.sh rebooted")
            if code == 0:
                return
            if code != 3:
                sys.exit("stopped: the server did not come back from its reboot")
            time.sleep(10)
        sys.exit("stopped: the server did not reboot")

    step("server", f"{NODE}/crash.sh start")
    if keys:
        step("loader", f"{NODE}/dataset.sh crash-preload {keys} {value_bytes}")
    names = []
    for i, (kind, fsync) in enumerate(cases, 1):
        name = f"{i}-{kind}-{fsync}"
        loss = kind == "reboot" and fsync != "always"  # everysec may lose about the last second
        step("server", f"{NODE}/crash.sh fsync {fsync}")
        writer = threading.Thread(
            target=step, args=("loader", f"{NODE}/crash_load.sh write {name} {write_s} {workers} {pipeline} {churn}"))
        writer.start()
        time.sleep(fault_at_s)
        if kind == "kill":
            step("server", f"{NODE}/crash.sh kill")
            step("server", f"{NODE}/crash.sh restart")
        else:
            step("server", f"{NODE}/crash.sh reboot")
            rebooted()
        writer.join()
        step("loader", f"{NODE}/crash_load.sh verify {name}" + (" allow-loss" if loss else ""))
        names.append((name, loss))
    step("server", f"{NODE}/crash.sh fsync everysec")
    for name, loss in names:
        step("loader", f"{NODE}/crash_load.sh verify {name} final" + (" allow-loss" if loss else ""))
    collect(outs)


def collect(outs):
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    dest = os.path.join(REPO, "bench", "results", f"cloud-{outs['run_id']}-{stamp}")
    sh(["aws", "s3", "sync", "--only-show-errors", "--region", outs["region"],
        f"s3://{outs['bucket']}/{outs['run_id']}/", dest], capture=False)
    print(f"results in {dest}")


def leftovers(region):
    """Prints anything of the test still in the account; exit 1 if there is something."""
    found = []
    tagged = aws("resourcegroupstaggingapi", "get-resources", "--tag-filters", "Key=project,Values=datadam-bench",
                 region=region)
    # Terminated instances, deleted volumes and deleted subnets stay listed
    # there for a while: those are asked of EC2 directly below.
    found += [m["ResourceARN"] for m in tagged.get("ResourceTagMappingList", [])
              if not any(kind in m["ResourceARN"] for kind in (":instance/", ":volume/", ":subnet/"))]
    subnets = aws("ec2", "describe-subnets", "--filters", "Name=tag:project,Values=datadam-bench", region=region)
    found += [f"subnet {s['SubnetId']}" for s in subnets.get("Subnets", [])]
    inst = aws("ec2", "describe-instances", "--filters", "Name=tag:project,Values=datadam-bench",
               "Name=instance-state-name,Values=pending,running,stopping,stopped", region=region)
    found += [f"instance {i['InstanceId']} ({i['State']['Name']})" for r in inst.get("Reservations", [])
              for i in r["Instances"]]
    vols = aws("ec2", "describe-volumes", "--filters", "Name=tag:project,Values=datadam-bench", region=region)
    found += [f"volume {v['VolumeId']} ({v['State']})" for v in vols.get("Volumes", [])]
    vols = aws("ec2", "describe-volumes", "--filters", "Name=status,Values=available", region=region)
    found += [f"unattached volume {v['VolumeId']} ({v['Size']} GB)" for v in vols.get("Volumes", [])]
    addrs = aws("ec2", "describe-addresses", region=region)
    found += [f"elastic IP {a['PublicIp']}" for a in addrs.get("Addresses", [])]
    spot = aws("ec2", "describe-spot-instance-requests", "--filters", "Name=state,Values=open,active", region=region)
    found += [f"spot request {s['SpotInstanceRequestId']}" for s in spot.get("SpotInstanceRequests", [])]
    buckets = aws("s3api", "list-buckets")
    found += [f"bucket {b['Name']}" for b in buckets.get("Buckets", []) if b["Name"].startswith("datadam-bench-")]
    roles = aws("iam", "list-roles")
    found += [f"IAM role {r['RoleName']}" for r in roles.get("Roles", []) if r["RoleName"].startswith("datadam-bench-")]
    profiles = aws("iam", "list-instance-profiles")
    found += [f"instance profile {p['InstanceProfileName']}" for p in profiles.get("InstanceProfiles", [])
              if p["InstanceProfileName"].startswith("datadam-bench-")]
    if found:
        print("left in the account:")
        for f in found:
            print(f"  {f}")
        sys.exit(1)
    print(f"nothing left in {region} (nor in IAM or S3)")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == "leftovers":
        return leftovers(sys.argv[2] if len(sys.argv) > 2 else "us-east-1")
    outs = tf_outputs()
    if cmd == "wait":
        wait_ready(outs)
    elif cmd == "run":
        if len(sys.argv) < 4:
            sys.exit("usage: cloud.py run <node> <command...>")
        code, _, _ = run_on(outs, sys.argv[2], " ".join(sys.argv[3:]))
        sys.exit(0 if code == 0 else 1)
    elif cmd == "smoke":
        wait_ready(outs)
        for node, command in [("server", "/opt/datadam/cloud/node/server.sh start datadam"),
                              ("loader", "/opt/datadam/cloud/node/smoke.sh")]:
            code, _, _ = run_on(outs, node, command)
            if code != 0:
                sys.exit(f"smoke failed on {node}")
        collect(outs)
    elif cmd == "memory":
        # python cloud/cloud.py memory [--pilot]: phase 2; the pilot checks the
        # whole loop in a few minutes on the same machines.
        if "--pilot" in sys.argv:
            scenario_memory(outs, reps=1, seconds=10, keys=200_000, servers=["datadam", "redis", "kvrocks"],
                            rates=RATES[:1])
        else:
            scenario_memory(outs, reps=3, seconds=60, keys=5_000_000, servers=["datadam", "redis", "kvrocks"],
                            rates=RATES)
    elif cmd == "large":
        # python cloud/cloud.py large [--pilot]: phase 3, datadam and Kvrocks
        # (Redis keeps everything in memory) with ~200 GB on a 64 GB machine.
        if "--pilot" in sys.argv:
            scenario_large(outs, keys=2_000_000, value_bytes=1000, seconds=10, settle_s=10, reps=1,
                           servers=["datadam", "kvrocks"], rates=LARGE_RATES[:1])
        else:
            scenario_large(outs, keys=200_000_000, value_bytes=1000, seconds=60, settle_s=300, reps=2,
                           servers=["datadam", "kvrocks"], rates=LARGE_RATES)
    elif cmd == "scale":
        # python cloud/cloud.py scale [--pilot]: phase 4, 4/8/16 vCPUs, in memory.
        stores = ["datadam", "redis", "kvrocks"]
        if "--pilot" in sys.argv:
            scenario_scale(outs, reps=1, seconds=10, keys=200_000, stores=stores, sweep=[1, 16])
        else:
            scenario_scale(outs, reps=2, seconds=45, keys=5_000_000, stores=stores, sweep=IO_THREADS)
    elif cmd == "scale-image":
        # python cloud/cloud.py scale-image <image>: phase 4 again for datadam
        # only, from another image, on the same machines (the nodes pull the
        # scripts first: DATADAM_IMAGE is newer than the run).
        image = sys.argv[2]
        for node, _ in SCALE_SERVERS:
            run_on(outs, node, "git -C /opt/datadam pull -q")
        scenario_scale(outs, reps=2, seconds=45, keys=5_000_000, stores=["datadam-new"], sweep=[4, 8, 16],
                       image=image)
    elif cmd == "ha":
        # python cloud/cloud.py ha [--pilot]: phase 5, writes with WAIT 1
        # through four faults; 10 GB for the full sync of the rejoining node.
        if "--pilot" in sys.argv:
            scenario_ha(outs, keys=200_000, value_bytes=1000, write_s=30, fault_at_s=10, cut_s=15, workers=8)
        else:
            scenario_ha(outs, keys=10_000_000, value_bytes=1000, write_s=90, fault_at_s=25, cut_s=30, workers=16)
    elif cmd == "crash":
        # python cloud/cloud.py crash [--pilot]: phase 6, kill -9 and hard reboots
        # under writes, over a data set larger than the caches.
        if "--pilot" in sys.argv:
            scenario_crash(outs, keys=500_000, value_bytes=1000, write_s=60, fault_at_s=15, workers=8, pipeline=16,
                           churn=2, cases=[("kill", "everysec"), ("reboot", "everysec"), ("reboot", "always")])
        else:
            scenario_crash(outs, keys=50_000_000, value_bytes=1000, write_s=300, fault_at_s=60, workers=16,
                           pipeline=16, churn=4,
                           cases=[("kill", "everysec"), ("kill", "everysec"), ("kill", "always"),
                                  ("reboot", "everysec"), ("reboot", "everysec"), ("reboot", "always"),
                                  ("reboot", "always")])
    elif cmd == "collect":
        collect(outs)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
