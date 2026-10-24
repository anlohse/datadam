# Phase 1: two small machines, to check the whole loop (apply, setup, run,
# collect, destroy, no leftovers) for well under a dollar.
#   terraform -chdir=cloud/terraform apply -var-file=../scenarios/smoke.tfvars
run_id    = "smoke"
max_hours = 2
nodes = {
  server = { role = "server", instance_type = "c7i.large" }
  loader = { role = "loader", instance_type = "c7i.large" }
}
