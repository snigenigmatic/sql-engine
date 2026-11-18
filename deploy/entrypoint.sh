#!/bin/sh
# Starts sqlnode in the role ROLE, working out its identity from the
# hostname the way StatefulSet pods are named (coord-0, node-2, ...).
#
#   ROLE          coordinator | storage
#   COORDINATORS  every coordinator: id=host:raft_port:client_port,...
#   ID            coordinator id (default: hostname ordinal + 1)
#   NAME          storage node name (default: the hostname)
#   ADVERTISE     how other processes reach this one (default: the hostname,
#                 plus "." and SUBDOMAIN if set: coord-0.coord)
#   SUBDOMAIN     the headless service (Kubernetes); empty with compose
#   SHARDS, RF    for a new cluster (coordinators; default 16, 3)
#   PORT          storage node port (default 7500)
#   RAFT_BASE     storage replicas listen on RAFT_BASE + shard (default 20000)
#   LOCK_TIMEOUT, TXN_IDLE_TIMEOUT, NODE_DOWN_AFTER   seconds (default 5, 60, 300)
# Data lives in /data. Anything else on the command line goes to sqlnode.
set -eu
host=$(hostname)
ordinal=${host##*-}
advertise=${ADVERTISE:-$host${SUBDOMAIN:+.$SUBDOMAIN}}
case "${ROLE:-}" in
  coordinator)
    exec sqlnode --role coordinator --id "${ID:-$((ordinal + 1))}" --coordinators "$COORDINATORS" \
      --dir /data --shards "${SHARDS:-16}" --rf "${RF:-3}" \
      --lock-timeout "${LOCK_TIMEOUT:-5}" --txn-idle-timeout "${TXN_IDLE_TIMEOUT:-60}" \
      --node-down-after "${NODE_DOWN_AFTER:-300}" "$@"
    ;;
  storage)
    exec sqlnode --role storage --name "${NAME:-$host}" --coordinators "$COORDINATORS" --dir /data \
      --host "$advertise" --listen 0.0.0.0 --port "${PORT:-7500}" --raft-base "${RAFT_BASE:-20000}" "$@"
    ;;
  *)
    echo "entrypoint: set ROLE to coordinator or storage" >&2
    exit 2
    ;;
esac
