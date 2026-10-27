# Phase 4: scaling with cores. The same family at 4, 8 and 16 vCPUs, data in
# memory, datadam x Redis x Kvrocks, then datadam's io-threads swept on each;
# one loader with twice the largest server's vCPUs. 60 vCPUs in all (the
# account's on-demand quota is 64).
#   terraform -chdir=cloud/terraform apply -var-file=../scenarios/scale.tfvars -var image=ghcr.io/anlohse/datadam:sha-<commit>
#   python cloud/cloud.py scale --pilot      (a few minutes: the loop works)
#   python cloud/cloud.py scale              (about 2 hours)
run_id    = "scale"
max_hours = 4
nodes = {
  s4     = { role = "server", instance_type = "i4i.xlarge", spot = false }
  s8     = { role = "server", instance_type = "i4i.2xlarge", spot = false }
  s16    = { role = "server", instance_type = "i4i.4xlarge", spot = false }
  loader = { role = "loader", instance_type = "c7i.8xlarge", spot = false }
}
