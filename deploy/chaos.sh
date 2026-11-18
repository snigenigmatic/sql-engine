#!/bin/bash
# Kill sqlengine processes at random while money moves between accounts,
# then check that none was created or lost (deploy/workload.sh).
#
#   deploy/chaos.sh [SECONDS] [EVERY]         Kubernetes: deletes a random pod
#                                             (--force: like a crash) every EVERY s
#   MODE=compose deploy/chaos.sh [SECONDS]    deploy/compose.yaml: docker kill,
#                                             then start it again
#
# Defaults: 120 s, a kill every 15 s. Exits non-zero if the total changed.
set -euo pipefail
cd "$(dirname "$0")/.."
DURATION=${1:-120}
EVERY=${2:-15}
MODE=${MODE:-kubernetes}
PODS=(coord-0 coord-1 coord-2 node-0 node-1 node-2)

if [ "$MODE" = compose ]; then
  COMPOSE=(docker compose -f deploy/compose.yaml ${COMPOSE_PROJECT:+-p "$COMPOSE_PROJECT"})
  "${COMPOSE[@]}" --profile client up -d client > /dev/null 2>&1
  client() { "${COMPOSE[@]}" exec -T client workload.sh "$@"; }
  crash() { "${COMPOSE[@]}" kill "$1" > /dev/null 2>&1; sleep 3; "${COMPOSE[@]}" start "$1" > /dev/null 2>&1; }
else
  K=(kubectl ${NAMESPACE:+-n "$NAMESPACE"})
  "${K[@]}" delete pod sql-client --ignore-not-found --wait > /dev/null
  "${K[@]}" run sql-client --image="${IMAGE:-sqlengine:dev}" --image-pull-policy=IfNotPresent --restart=Never \
    --command -- sleep infinity > /dev/null
  "${K[@]}" wait --for=condition=Ready pod/sql-client --timeout=120s > /dev/null
  client() { "${K[@]}" exec sql-client -- workload.sh "$@"; }
  crash() {
    "${K[@]}" delete pod "$1" --grace-period=0 --force > /dev/null 2>&1
    sleep 5
    "${K[@]}" wait --for=condition=Ready pod/"$1" --timeout=180s > /dev/null
  }
fi

[ -n "$(client total)" ] || client setup 100 > /dev/null
before=$(client total)
echo "before: $before (accounts | total)"
log=$(mktemp)
client transfers "$DURATION" > "$log" 2>&1 &
workload=$!
end=$(($(date +%s) + DURATION - EVERY))
while [ "$(date +%s)" -lt "$end" ]; do
  sleep "$EVERY"
  victim=${PODS[$((RANDOM % ${#PODS[@]}))]}
  echo "$(date +%T) killing $victim"
  crash "$victim"
  echo "$(date +%T) $victim is back"
done
wait "$workload" || true
cat "$log"
after=$(client total)
echo "after:  $after"
if [ "$before" != "$after" ]; then
  echo "FAILED: the total changed" >&2
  exit 1
fi
echo "OK: the total did not change"
