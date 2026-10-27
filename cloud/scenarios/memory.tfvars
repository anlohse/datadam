# Phase 2: data in memory. datadam, Redis and Kvrocks in turn on the same
# server (8 vCPUs, 64 GB, NVMe), driven by a loader twice its size.
#   terraform -chdir=cloud/terraform apply -var-file=../scenarios/memory.tfvars -var image=ghcr.io/anlohse/datadam:sha-<commit>
#   python cloud/cloud.py memory --pilot      (a few minutes: the loop works)
#   python cloud/cloud.py memory              (about 1.5 hours: 3 servers x 3 repetitions)
run_id    = "memory"
max_hours = 4
nodes = {
  # On demand: a Spot i4i.2xlarge was taken back seconds after launch on the first try.
  server = { role = "server", instance_type = "i4i.2xlarge", spot = false }
  loader = { role = "loader", instance_type = "c7i.4xlarge" }
}
