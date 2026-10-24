# Cloud tests on AWS

A throwaway environment for testing datadam on real machines: Terraform creates it for a test run, the
scripts run the scenarios and bring the results back, and `terraform destroy` removes all of it. Nothing is
meant to stay in the account, and several mechanisms make sure nothing does (see
[Nothing left behind](#nothing-left-behind)).

## Contents

- [What gets created](#what-gets-created)
- [One-time setup](#one-time-setup)
- [Running a test](#running-a-test)
- [Nothing left behind](#nothing-left-behind)
- [Phases](#phases)
- [Costs](#costs)
- [Files](#files)

## What gets created

- A VPC (`10.42.0.0/16`) with a public subnet in each of 3 availability zones and an internet gateway. No NAT
  gateway (it is billed by the hour).
- A security group that admits traffic only from the other test nodes. Nothing is open to the internet: the
  public addresses are for outbound traffic only (pulling images, SSM, S3).
- The nodes, from the `nodes` variable (one `scenarios/*.tfvars` per phase): Ubuntu 24.04, Spot by default,
  with a fixed private address each (`10.42.<az>.<10 + position>`), in one cluster placement group when they
  are all in one zone. Roles:
  - `server`: runs datadam, Redis or Kvrocks, one at a time, started by the scenario
    (`cloud/node/server.sh`), on the instance's NVMe store when it has one.
  - `loader`: memtier_benchmark and redis-benchmark; runs the scenario.
  - `replica`: a datadam replica of the node named in `follows`, started at boot.
  - `sentinel`: redis-sentinel watching the node in `follows`, started at boot.
- An IAM role for the nodes: SSM Session Manager and access to the results bucket only.
- An S3 bucket for the results, with `force_destroy`: destroy removes it with its contents.
- A random password, used as `requirepass` everywhere (so protected mode lets the nodes in).

Every node clones this repository at `repo_ref` (default `main`) and runs `cloud/node/setup.sh` for its role,
and runs the image `image` (default `ghcr.io/anlohse/datadam:latest`; pin `:sha-<commit>` for results you keep).

## One-time setup

**On AWS** (in the console):

1. **A login for the command line, not the root user.** Easiest and safest: IAM Identity Center. Enable it,
   create a user for yourself and give it the `AdministratorAccess` permission set on your account. The command
   line then gets short-lived credentials through `aws sso login`.
2. **A budget alert.** Billing and Cost Management → Budgets → Create a cost budget, for example US$ 50 a month,
   with e-mail alerts at 50%, 80% and 100%. The first two budgets are free.
3. **vCPU quota.** Service Quotas → Amazon EC2 → "All Standard (A, C, D, H, I, M, R, T, Z) Spot Instance
   Requests" and "Running On-Demand Standard (A, C, D, H, I, M, R, T, Z) instances": new accounts often allow
   only a few vCPUs. Ask for 64 for both in us-east-1. The answer can take from minutes to a couple of days, so
   ask early. The smoke test needs only 4.

**On your machine** (Windows; the same tools exist for Linux and macOS):

```bash
winget install Hashicorp.Terraform
winget install Amazon.AWSCLI
winget install Amazon.SessionManagerPlugin
```

Then log in once and make that profile the default for these commands:

```bash
aws configure sso                     # start URL and region from IAM Identity Center; name the profile datadam
export AWS_PROFILE=datadam            # in each new terminal (PowerShell: $env:AWS_PROFILE = "datadam")
aws sts get-caller-identity           # should print your account
```

The scripts also need Python 3 (standard library only).

## Running a test

From the repository root, for the smoke test (phase 1):

```bash
aws sso login
terraform -chdir=cloud/terraform init
terraform -chdir=cloud/terraform apply -var-file=../scenarios/smoke.tfvars
python cloud/cloud.py smoke           # waits for the nodes, starts datadam, runs cloud/node/smoke.sh, collects
terraform -chdir=cloud/terraform destroy -var-file=../scenarios/smoke.tfvars
python cloud/cloud.py leftovers       # must print "nothing left"
```

Other commands, while the environment exists:

| Command | What it does |
|---|---|
| `python cloud/cloud.py wait` | Waits until every node answers SSM and finished its setup |
| `python cloud/cloud.py run <node> <command...>` | Runs a shell command on a node as root and prints its output, e.g. `run server server.sh info persistence` |
| `python cloud/cloud.py collect` | Copies the run's results from the bucket to `bench/results/cloud-<run>-<time>/` |
| `aws ssm start-session --target <instance id>` | A shell on a node (`terraform output connect` prints them) |
| `terraform -chdir=cloud/terraform output -raw password` | The `requirepass` of the nodes |

The node scripts are in `cloud/node/`: `server.sh start datadam|redis|kvrocks [args]` sizes each store for the
machine (datadam: 45% of the memory as object cache, 15% as block cache, `io-threads` = CPUs);
`load.sh <name> [memtier args]` runs memtier against the server and saves its JSON and text plus the server's
`INFO` before and after; `sysstats.sh start <name>` / `stop` records `mpstat` and `iostat` every second.

## Nothing left behind

- **Everything comes from Terraform, and the state is local** (`cloud/terraform/terraform.tfstate`, not
  committed): no state bucket stays in the account. Keep that file until you have destroyed the environment.
- **destroy removes it all**, including the bucket and its contents (`force_destroy`), the instances' disks
  (`delete_on_termination`), and the NVMe stores (they belong to the instance). Nothing is created that
  outlives it: no Elastic IPs, snapshots, NAT gateways, container registries or log groups.
- **Instances end themselves.** Each one runs `shutdown -h +<max_hours × 60>` at boot, and shutting down
  terminates it (`instance_initiated_shutdown_behavior = "terminate"`). If you forget `destroy`, or your
  machine goes away mid-test, they stop costing after `max_hours`; the rest (VPC, bucket, role) costs nothing
  idle, and `destroy` removes it later.
- **`cloud.py leftovers` checks.** Everything is tagged `project = datadam-bench`. The check lists what still has
  that tag, plus running or stopped instances with it, unattached volumes, Elastic IPs, open Spot requests,
  `datadam-bench-*` buckets, IAM roles and instance profiles. An empty list means nothing is billed.
- **The budget alert** is the last net.

## Phases

| Phase | Scenario | What it shows |
|---|---|---|
| 1. Smoke | `smoke.tfvars`: 2 × c7i.large | The loop works: apply, setup, run, collect, destroy, no leftovers |
| 2. In memory | server i4i.2xlarge, loader c7i.4xlarge | datadam × Redis × Kvrocks, the 5 workloads, distinct seeds, 3 alternated repetitions; 50/200/1000 connections; pipelines |
| 3. Larger than memory | same machines, 200–500 GB of data | Load time, uniform and gaussian reads, mixed, writes, flush stalls; datadam × Kvrocks |
| 4. Scaling with cores | i4i.xlarge, 2xlarge, 4xlarge | Throughput per core; `io-threads` sweep |
| 5. High availability | primary + 2 replicas in 3 zones, 3 sentinels, a loader | Writes with `WAIT 1` while the primary is killed or cut off: time without writes, acknowledged writes lost (should be 0), full sync and catch-up times; planned `FAILOVER` |
| 6. Crash | server + loader | `kill -9` and reboot under load, then every key checked |
| 7. Soak | server + loader, 8–12 h | Memory growth, compaction debt, latency drift |

The scenarios of phases 2 to 7 are added as each phase is prepared.

## Costs

Approximate on-demand prices in us-east-1 (check the AWS pricing calculator); Spot is usually 60–70% less:

| Instance | vCPUs / memory | US$ per hour |
|---|---|---|
| c7i.large | 2 / 4 GB | ~0.09 |
| c7i.4xlarge | 16 / 32 GB | ~0.71 |
| i4i.xlarge | 4 / 32 GB, 937 GB NVMe | ~0.34 |
| i4i.2xlarge | 8 / 64 GB, 1.9 TB NVMe | ~0.69 |
| i4i.4xlarge | 16 / 128 GB, 3.8 TB NVMe | ~1.37 |

The smoke test costs a few cents. All seven phases, on demand, come to roughly US$ 40; with Spot, well under
half. Traffic between the nodes in one zone is free; across zones (phase 5) it costs US$ 0.01/GB each way.
Each running instance also pays for its public IPv4 address (about US$ 0.005 an hour); it is released with the
instance.

## Files

| Path | What it is |
|---|---|
| `terraform/` | The environment (`versions.tf`, `variables.tf`, `main.tf`, `outputs.tf`) and the boot script template `cloud-init.sh.tftpl` |
| `scenarios/*.tfvars` | The machines of each phase |
| `node/` | What runs on the nodes: `setup.sh` (first boot), `server.sh`, `replica.sh`, `sentinel.sh`, `load.sh`, `sysstats.sh`, and the scenarios (`smoke.sh`) |
| `cloud.py` | What runs on your machine: wait, run, the scenarios, collect, leftovers |
