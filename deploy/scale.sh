#!/bin/bash
# Set the number of storage nodes and wait until every shard's replicas are
# where the placement wants them (the data stays available meanwhile).
#
#   deploy/scale.sh N                 Kubernetes (StatefulSet "node")
#   MODE=compose deploy/scale.sh 3|4  deploy/compose.yaml (node-3 is the extra one)
#
# Going down, the extra nodes are first taken out of the cluster
# (.cluster remove: their replicas move to the others), then stopped.
# Going up, a name removed before is let back in (.cluster add).
set -euo pipefail
cd "$(dirname "$0")/.."
WANT=${1:?usage: deploy/scale.sh NODES}
MODE=${MODE:-kubernetes}

if [ "$MODE" = compose ]; then
  COMPOSE=(docker compose -f deploy/compose.yaml ${COMPOSE_PROJECT:+-p "$COMPOSE_PROJECT"})
  "${COMPOSE[@]}" --profile client up -d client > /dev/null 2>&1
  admin() { "${COMPOSE[@]}" exec -T client sh -c "printf '%s\n' '$1' | sqlengine --connect \$SQL_ADDRESSES"; }
  have=$(("$(admin .cluster | grep -c '^  node-[0-9]* (id [0-9]*) .* active')"))
  start() { "${COMPOSE[@]}" --profile scale up -d "node-$1" > /dev/null 2>&1; }
  stop() { "${COMPOSE[@]}" --profile scale rm -sf "node-$1" > /dev/null 2>&1; }
else
  K=(kubectl ${NAMESPACE:+-n "$NAMESPACE"})
  admin() { "${K[@]}" exec coord-0 -- sh -c "printf '%s\n' '$1' | sqlengine --connect 127.0.0.1:7480"; }
  have=$("${K[@]}" get statefulset node -o jsonpath='{.spec.replicas}')
  start() { "${K[@]}" scale statefulset node --replicas="$WANT" > /dev/null; }
  stop() { "${K[@]}" scale statefulset node --replicas="$WANT" > /dev/null; }
fi

wait_balanced() {
  for _ in $(seq 1 120); do
    if admin .cluster | grep -q '^Balanced'; then
      admin .cluster
      return 0
    fi
    sleep 2
  done
  admin .cluster
  echo "scale: not balanced after 4 minutes" >&2
  return 1
}

if [ "$WANT" -lt "$have" ]; then
  for n in $(seq "$WANT" $((have - 1))); do
    admin ".cluster remove node-$n"
  done
  # Drained when the removed nodes hold no replica any more
  wait_balanced
  for n in $(seq "$WANT" $((have - 1))); do stop "$n"; done
elif [ "$WANT" -gt "$have" ]; then
  for n in $(seq "$have" $((WANT - 1))); do
    admin ".cluster add node-$n" > /dev/null || true # in case it was removed before
    start "$n"
  done
  sleep 5 # let them announce themselves
  wait_balanced
fi
echo "storage nodes: $WANT"
