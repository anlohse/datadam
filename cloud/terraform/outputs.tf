output "region" {
  value = var.region
}

output "run_id" {
  value = var.run_id
}

output "bucket" {
  description = "Where the scenarios upload their results (removed by destroy)."
  value       = aws_s3_bucket.results.id
}

output "nodes" {
  description = "name => instance id, role and private address."
  value = { for n, i in aws_instance.node : n => {
    id   = i.id
    role = var.nodes[n].role
    ip   = i.private_ip
  } }
}

output "password" {
  description = "datadam's requirepass on every node."
  value       = random_password.datadam.result
  sensitive   = true
}

output "connect" {
  description = "A shell on each node, through SSM."
  value       = { for n, i in aws_instance.node : n => "aws ssm start-session --region ${var.region} --target ${i.id}" }
}
