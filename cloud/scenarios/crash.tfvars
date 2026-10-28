# Phase 6: crash. datadam on the server of phases 2 and 3 (8 vCPUs, 64 GB, NVMe),
# killed (kill -9) and hard-rebooted (sysrq b, as a power cut) under writes.
#   terraform -chdir=cloud/terraform apply -var-file=../scenarios/crash.tfvars -var image=ghcr.io/anlohse/datadam:sha-<commit>
#   python cloud/cloud.py crash --pilot      (about 15 minutes: the loop works)
#   python cloud/cloud.py crash              (about an hour: 50 GB, seven crashes)
run_id    = "crash"
max_hours = 3
# On demand: a node taken back would be a crash of its own.
nodes = {
  server = { role = "server", instance_type = "i4i.2xlarge", spot = false }
  loader = { role = "loader", instance_type = "c7i.4xlarge", spot = false }
}
