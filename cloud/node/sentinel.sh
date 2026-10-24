#!/bin/bash
# On a sentinel node (at boot): redis-sentinel watching the primary at $FOLLOWS
# (master name "dd", quorum 2 of 3).
. /opt/datadam/cloud/node/common.sh

mkdir -p /data/sentinel
cat > /data/sentinel/sentinel.conf <<EOF
port 26379
sentinel monitor dd $FOLLOWS 6379 2
sentinel auth-pass dd $PASSWORD
sentinel down-after-milliseconds dd 3000
sentinel failover-timeout dd 10000
sentinel parallel-syncs dd 1
EOF
chmod -R 777 /data/sentinel
docker rm -f bench-sentinel >/dev/null 2>&1 || true
docker run -d --name bench-sentinel --network host --restart unless-stopped -v /data/sentinel:/data redis:7.4 \
  redis-sentinel /data/sentinel.conf >/dev/null
