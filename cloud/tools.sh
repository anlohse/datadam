#!/usr/bin/env bash
# Runs a command in the cloud tools image (AWS CLI v2, Terraform, Python),
# with the repository at /work and ~/.aws mounted; builds the image first.
#   bash cloud/tools.sh aws sts get-caller-identity
#   bash cloud/tools.sh terraform -chdir=cloud/terraform apply -var-file=../scenarios/smoke.tfvars
#   bash cloud/tools.sh python3 cloud/cloud.py smoke
# env: EXTRA_CA (a root CA to trust, e.g. an antivirus's), AWS_PROFILE,
#      AWS_REGION (default us-east-1), REBUILD=1 (build the image again)
set -euo pipefail
export MSYS_NO_PATHCONV=1  # Git Bash on Windows: keep the container paths as they are

host_path() { (cd "$1" && (pwd -W 2>/dev/null || pwd)); }
REPO=$(host_path "$(dirname "$0")/..")
AWS_DIR=${AWS_DIR:-$HOME/.aws}
mkdir -p "$AWS_DIR"
IMAGE=datadam-cloud-tools

if [ -n "${REBUILD:-}" ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  build=(docker build -q -t "$IMAGE")
  [ -n "${EXTRA_CA:-}" ] && build+=(--secret "id=extra_ca,src=$EXTRA_CA")
  "${build[@]}" "$REPO/cloud/tools" >/dev/null
fi

args=(--rm -i -v "$REPO":/work -v "$(host_path "$AWS_DIR")":/root/.aws -w /work
      -e AWS_REGION="${AWS_REGION:-us-east-1}" -e AWS_PROFILE)
[ -t 0 ] && [ -t 1 ] && args+=(-t)
[ -n "${EXTRA_CA:-}" ] && args+=(-v "$EXTRA_CA":/extra-ca.crt:ro)
# Windows editors like to save the credentials file as credentials.txt.
if [ ! -f "$AWS_DIR/credentials" ] && [ -f "$AWS_DIR/credentials.txt" ]; then
  args+=(-e AWS_SHARED_CREDENTIALS_FILE=/root/.aws/credentials.txt)
fi
exec docker run "${args[@]}" "$IMAGE" "$@"
