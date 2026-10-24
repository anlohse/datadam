#!/bin/bash
# Entry point of the tools image: trusts the extra root CA mounted at
# /extra-ca.crt (an antivirus that intercepts HTTPS, for instance), then runs
# the command.
set -euo pipefail
if [ -s /extra-ca.crt ]; then
  cat /etc/pki/tls/certs/ca-bundle.crt /extra-ca.crt > /tmp/ca-bundle.pem
  export SSL_CERT_FILE=/tmp/ca-bundle.pem AWS_CA_BUNDLE=/tmp/ca-bundle.pem
fi
exec "$@"
