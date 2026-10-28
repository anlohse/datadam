# Phase 7: soak. datadam on the server of phases 2, 3 and 6 (8 vCPUs, 64 GB, NVMe),
# with 60 GB loaded, under hours of steady load at about half its capacity.
#   terraform -chdir=cloud/terraform apply -var-file=../scenarios/soak.tfvars -var image=ghcr.io/anlohse/datadam:sha-<commit>
#   python cloud/cloud.py soak --pilot      (about 30 minutes: the loop works)
#   python cloud/cloud.py soak              (starts 10 hours of load; the nodes run it)
#   python cloud/cloud.py soak-status / soak-stop
run_id    = "soak"
# The run, the load before it, and room to collect; the instances end themselves after this.
max_hours = 12
nodes = {
  server = { role = "server", instance_type = "i4i.2xlarge", spot = false }
  loader = { role = "loader", instance_type = "c7i.2xlarge", spot = false }
}
