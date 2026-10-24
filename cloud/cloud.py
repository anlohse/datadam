#!/usr/bin/env python3
"""Runs the cloud test from this machine: commands on the nodes (through SSM),
results back from the bucket, and a check that nothing is left after
`terraform destroy`. Needs the AWS CLI v2 (logged in) and Terraform.

  python cloud/cloud.py wait                     every node up and set up
  python cloud/cloud.py run <node> <command...>  run a shell command on a node, print its output
  python cloud/cloud.py smoke                    phase 1: start datadam, run cloud/node/smoke.sh on the loader
  python cloud/cloud.py collect                  results of the run -> bench/results/cloud-<run>-<time>/
  python cloud/cloud.py leftovers [region]       anything of datadam-bench still in the account?
"""

import json
import os
import subprocess
import sys
import tempfile
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
    # Terminated instances stay listed for a while: instances are checked by state below.
    found += [m["ResourceARN"] for m in tagged.get("ResourceTagMappingList", []) if ":instance/" not in m["ResourceARN"]]
    inst = aws("ec2", "describe-instances", "--filters", "Name=tag:project,Values=datadam-bench",
               "Name=instance-state-name,Values=pending,running,stopping,stopped", region=region)
    found += [f"instance {i['InstanceId']} ({i['State']['Name']})" for r in inst.get("Reservations", [])
              for i in r["Instances"]]
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
    elif cmd == "collect":
        collect(outs)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
