#!/bin/bash
# Build the image, load it into the local Kubernetes cluster, deploy
# sqlengine and wait until every pod is ready.
#
#   deploy/up.sh             minikube (started if it is not running)
#   KIND=1 deploy/up.sh      kind (the current kind cluster)
#
# Then:  kubectl exec -it coord-0 -- sqlengine --connect coord-0.coord:7480,coord-1.coord:7480,coord-2.coord:7480
#   or:  kubectl port-forward svc/sql 7480  and  build/src/sqlengine --connect 127.0.0.1:7480
# Remove: kubectl delete -f deploy/k8s/sqlengine.yaml && kubectl delete pvc -l app.kubernetes.io/name=sqlengine
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE=${IMAGE:-sqlengine:dev}

if [ -z "${KIND:-}" ] && ! minikube status > /dev/null 2>&1; then
  minikube start ${MINIKUBE_ARGS:-}
fi
docker build -f deploy/Dockerfile -t "$IMAGE" ${DOCKER_BUILD_ARGS:-} .
if [ -n "${KIND:-}" ]; then
  kind load docker-image "$IMAGE" ${KIND_CLUSTER:+--name "$KIND_CLUSTER"}
else
  minikube image load "$IMAGE"
fi
kubectl apply -f deploy/k8s/sqlengine.yaml
kubectl rollout status statefulset/coord --timeout=300s
kubectl rollout status statefulset/node --timeout=300s
kubectl wait --for=condition=Ready pod -l app.kubernetes.io/name=sqlengine --timeout=300s
kubectl exec coord-0 -- sh -c "printf '.help\n' | sqlengine --connect 127.0.0.1:7480" | grep '^Cluster:'
