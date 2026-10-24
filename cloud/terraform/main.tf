# A throwaway test environment for datadam: everything here is created for a
# test run and removed by `terraform destroy`. Nothing listens on the
# internet: the security group only lets the nodes talk to each other, and
# people reach the nodes through SSM Session Manager.

data "aws_availability_zones" "available" {
  state = "available"
  filter {
    name   = "opt-in-status"
    values = ["opt-in-not-required"]
  }
}

# Canonical's current Ubuntu 24.04 image, from AWS's public parameter store.
data "aws_ssm_parameter" "ubuntu" {
  name = "/aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id"
}

resource "random_id" "suffix" {
  byte_length = 4
}

# The password every node uses for datadam (and Redis, Kvrocks): with it,
# protected mode lets the other nodes in.
resource "random_password" "datadam" {
  length  = 32
  special = false
}

locals {
  name  = "datadam-bench-${var.run_id}"
  azs   = slice(data.aws_availability_zones.available.names, 0, 3)
  names = sort(keys(var.nodes))
  # Fixed private addresses, so a replica or a sentinel can be told its primary's
  # address before the primary exists: 10.42.<az>.<10 + position>.
  ips = { for i, n in local.names : n => cidrhost(aws_subnet.public[var.nodes[n].az].cidr_block, 10 + i) }
}

# ---- network: one VPC, a public subnet per AZ, no NAT gateway (it costs by the hour)

resource "aws_vpc" "main" {
  cidr_block           = "10.42.0.0/16"
  enable_dns_hostnames = true
  tags                 = { Name = local.name }
}

resource "aws_internet_gateway" "main" {
  vpc_id = aws_vpc.main.id
  tags   = { Name = local.name }
}

resource "aws_subnet" "public" {
  count                   = 3
  vpc_id                  = aws_vpc.main.id
  availability_zone       = local.azs[count.index]
  cidr_block              = "10.42.${count.index}.0/24"
  map_public_ip_on_launch = true # outbound only (image pulls, SSM): the security group admits no one from outside
  tags                    = { Name = "${local.name}-${count.index}" }
}

resource "aws_route_table" "public" {
  vpc_id = aws_vpc.main.id
  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.main.id
  }
  tags = { Name = local.name }
}

resource "aws_route_table_association" "public" {
  count          = 3
  subnet_id      = aws_subnet.public[count.index].id
  route_table_id = aws_route_table.public.id
}

resource "aws_security_group" "nodes" {
  name        = local.name
  description = "datadam test nodes: only each other"
  vpc_id      = aws_vpc.main.id

  ingress {
    description = "the other nodes"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    self        = true
  }
  egress {
    description = "image pulls, package installs, SSM, S3"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
}

resource "aws_placement_group" "cluster" {
  count    = var.cluster_placement ? 1 : 0
  name     = local.name
  strategy = "cluster"
}

# ---- results: one bucket per run, emptied and removed by destroy

resource "aws_s3_bucket" "results" {
  bucket        = "${local.name}-${random_id.suffix.hex}"
  force_destroy = true
}

resource "aws_s3_bucket_public_access_block" "results" {
  bucket                  = aws_s3_bucket.results.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

# ---- what the nodes may do: be reached by SSM, write results

resource "aws_iam_role" "node" {
  name = local.name
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { Service = "ec2.amazonaws.com" }
      Action    = "sts:AssumeRole"
    }]
  })
}

resource "aws_iam_role_policy_attachment" "ssm" {
  role       = aws_iam_role.node.name
  policy_arn = "arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore"
}

resource "aws_iam_role_policy" "results" {
  name = "results"
  role = aws_iam_role.node.id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect   = "Allow"
      Action   = ["s3:PutObject", "s3:GetObject", "s3:ListBucket"]
      Resource = [aws_s3_bucket.results.arn, "${aws_s3_bucket.results.arn}/*"]
    }]
  })
}

resource "aws_iam_instance_profile" "node" {
  name = local.name
  role = aws_iam_role.node.name
}

# ---- the nodes

resource "aws_instance" "node" {
  for_each = var.nodes

  ami                    = data.aws_ssm_parameter.ubuntu.value
  instance_type          = each.value.instance_type
  subnet_id              = aws_subnet.public[each.value.az].id
  private_ip             = local.ips[each.key]
  vpc_security_group_ids = [aws_security_group.nodes.id]
  iam_instance_profile   = aws_iam_instance_profile.node.name
  placement_group        = var.cluster_placement ? aws_placement_group.cluster[0].id : null

  # `shutdown` (from max_hours in cloud-init, or by hand) terminates the instance.
  instance_initiated_shutdown_behavior = "terminate"

  dynamic "instance_market_options" {
    for_each = var.use_spot ? [1] : []
    content {
      market_type = "spot"
      spot_options {
        spot_instance_type             = "one-time"
        instance_interruption_behavior = "terminate"
      }
    }
  }

  root_block_device {
    volume_type           = "gp3"
    volume_size           = var.root_volume_gb
    encrypted             = true
    delete_on_termination = true
  }

  metadata_options {
    http_tokens                 = "required"
    http_put_response_hop_limit = 2 # containers may reach the instance role
  }

  user_data = templatefile("${path.module}/cloud-init.sh.tftpl", {
    node      = each.key
    role      = each.value.role
    image     = var.image
    password  = random_password.datadam.result
    max_hours = var.max_hours
    bucket    = aws_s3_bucket.results.id
    region    = var.region
    run_id    = var.run_id
    repo_ref  = var.repo_ref
    follows   = each.value.follows == "" ? "" : local.ips[each.value.follows]
    nodes     = join(" ", [for n in local.names : "${n}=${var.nodes[n].role}=${local.ips[n]}"])
  })
  user_data_replace_on_change = true

  tags = {
    Name = "${local.name}-${each.key}"
    role = each.value.role
  }

  depends_on = [aws_route_table_association.public, aws_iam_role_policy_attachment.ssm]
}
