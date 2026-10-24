variable "region" {
  description = "AWS region. us-east-1 is among the cheapest and has every instance type used here."
  type        = string
  default     = "us-east-1"
}

variable "run_id" {
  description = "Names this test run in tags, the results bucket and the results directory."
  type        = string
  default     = "smoke"
  validation {
    condition     = can(regex("^[a-z0-9-]{1,20}$", var.run_id))
    error_message = "run_id: lower-case letters, digits and dashes, at most 20."
  }
}

variable "image" {
  description = "datadam image; pin a commit (ghcr.io/anlohse/datadam:sha-<commit>) so the results say what they measured."
  type        = string
  default     = "ghcr.io/anlohse/datadam:latest"
}

variable "repo_ref" {
  description = "Commit or branch of github.com/anlohse/datadam whose cloud/node scripts the nodes run."
  type        = string
  default     = "main"
}

variable "nodes" {
  description = <<-EOT
    The machines, by name. role: server (datadam, Redis or Kvrocks, started by the
    scenario), loader (memtier, redis-benchmark; runs the scenario), replica (a datadam
    replica of the node named in `follows`), sentinel (redis-sentinel watching `follows`).
    az: index of the availability zone (0-2).
  EOT
  type = map(object({
    role          = string
    instance_type = string
    az            = optional(number, 0)
    follows       = optional(string, "")
  }))
  default = {
    server = { role = "server", instance_type = "c7i.large" }
    loader = { role = "loader", instance_type = "c7i.large" }
  }
  validation {
    condition     = alltrue([for n in values(var.nodes) : contains(["server", "loader", "replica", "sentinel"], n.role)])
    error_message = "role must be server, loader, replica or sentinel."
  }
}

variable "use_spot" {
  description = "Spot instances: about a third of the on-demand price; AWS may take one back mid-run (the run is then repeated)."
  type        = bool
  default     = true
}

variable "cluster_placement" {
  description = "Put every node in one cluster placement group (same rack, lowest latency). Needs every node in az 0."
  type        = bool
  default     = true
}

variable "max_hours" {
  description = "Each instance shuts itself down (and is terminated) after this many hours, even if nobody runs terraform destroy."
  type        = number
  default     = 4
  validation {
    condition     = var.max_hours >= 1 && var.max_hours <= 24
    error_message = "max_hours: 1 to 24."
  }
}

variable "root_volume_gb" {
  description = "Root disk of each instance (gp3). Data goes to the instance's NVMe store when it has one, else here."
  type        = number
  default     = 30
}
