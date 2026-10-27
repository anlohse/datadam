#!/bin/bash
# On a replica node (at boot): a datadam replica of $FOLLOWS, with the same password.
. /opt/datadam/cloud/node/common.sh

docker rm -f bench-server >/dev/null 2>&1 || true
rm -rf /data/datadam
mkdir -p /data/datadam
chmod 777 /data/datadam
docker run -d --name bench-server --network host --restart unless-stopped --ulimit nofile=1048576:1048576 \
  -v /data/datadam:/var/lib/datadam "$IMAGE" \
  --requirepass "$PASSWORD" --masterauth "$PASSWORD" --replicaof "$FOLLOWS 6379" --memcache-port 0 \
  --wal-retention 1h "$@" >/dev/null
