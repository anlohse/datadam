#!/bin/bash
# First boot (from cloud-init): system tuning, then what the node's role needs.
. /opt/datadam/cloud/node/common.sh

# As a Redis host would be tuned: deep accept queues, no transparent huge pages
# (latency spikes), overcommit (Redis's fork).
sysctl -q -w net.core.somaxconn=65535 net.ipv4.tcp_max_syn_backlog=65535 vm.overcommit_memory=1
echo never > /sys/kernel/mm/transparent_hugepage/enabled

case $ROLE in
  server)
    docker pull -q "$IMAGE"
    docker pull -q redis:7.4
    docker pull -q apache/kvrocks:latest
    ;;
  replica)
    docker pull -q "$IMAGE"
    docker pull -q redis:7.4
    /opt/datadam/cloud/node/replica.sh
    ;;
  sentinel)
    docker pull -q redis:7.4
    /opt/datadam/cloud/node/sentinel.sh
    ;;
  loader)
    docker pull -q redis:7.4
    docker pull -q redislabs/memtier_benchmark:latest
    docker pull -q python:3.12-slim
    ;;
esac
