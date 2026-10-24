terraform {
  required_version = ">= 1.6"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.70"
    }
    random = {
      source  = "hashicorp/random"
      version = "~> 3.6"
    }
  }
  # State stays on this machine (terraform.tfstate here): no state bucket is
  # left behind in the account after `terraform destroy`.
}

provider "aws" {
  region = var.region
  # Every resource gets these tags; cloud/scripts/check_leftovers.sh looks for them.
  default_tags {
    tags = {
      project = "datadam-bench"
      run     = var.run_id
    }
  }
}
