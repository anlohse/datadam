# Phase 5: high availability. A primary and two replicas, one per availability
# zone; three sentinels (quorum 2), one per zone; a loader in the primary's zone.
#   terraform -chdir=cloud/terraform apply -var-file=../scenarios/ha.tfvars -var image=ghcr.io/anlohse/datadam:sha-<commit>
#   python cloud/cloud.py ha --pilot      (about 10 minutes: the loop works)
#   python cloud/cloud.py ha              (about 40 minutes: 10 GB, four faults)
run_id    = "ha"
max_hours = 3
# Three zones: no cluster placement group. On demand: a node taken back would be a fault of its own.
cluster_placement = false
use_spot          = false
nodes = {
  server   = { role = "server", instance_type = "i4i.xlarge", az = 0 }
  replica1 = { role = "replica", instance_type = "i4i.xlarge", az = 1, follows = "server" }
  replica2 = { role = "replica", instance_type = "i4i.xlarge", az = 2, follows = "server" }
  sentinel1 = { role = "sentinel", instance_type = "c7i.large", az = 0, follows = "server" }
  sentinel2 = { role = "sentinel", instance_type = "c7i.large", az = 1, follows = "server" }
  sentinel3 = { role = "sentinel", instance_type = "c7i.large", az = 2, follows = "server" }
  loader   = { role = "loader", instance_type = "c7i.2xlarge", az = 0 }
}
