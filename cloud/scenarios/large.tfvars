# Phase 3: larger than memory. datadam and Kvrocks in turn on the server of
# phase 2 (8 vCPUs, 64 GB, 1.9 TB NVMe) with ~200 GB of incompressible values.
#   terraform -chdir=cloud/terraform apply -var-file=../scenarios/large.tfvars -var image=ghcr.io/anlohse/datadam:sha-<commit>
#   python cloud/cloud.py large --pilot      (a few minutes: the loop works)
#   python cloud/cloud.py large              (several hours: most of it loading)
run_id    = "large"
max_hours = 8
nodes = {
  server = { role = "server", instance_type = "i4i.2xlarge", spot = false }
  loader = { role = "loader", instance_type = "c7i.4xlarge", spot = false }
}
