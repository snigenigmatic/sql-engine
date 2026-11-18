# Deploying sqlengine

One image, `sqlengine:dev`, runs every process of a cluster: `sqlnode` in the role given by `ROLE` (see `entrypoint.sh`), plus the `sqlengine` CLI. A cluster is 3 coordinators (`coord-0..2`, a Raft group that plans queries and keeps the metadata) and storage nodes (`node-0..2`, each holding replicas of the shards placed on it). Every process keeps its Raft logs and data in `/data`.

| File | What it is |
|---|---|
| `Dockerfile` | Multi-stage build: compile with CMake, then a 120 MB Ubuntu runtime with `sqlnode`, `sqlengine`, `entrypoint.sh`, `workload.sh` |
| `entrypoint.sh` | Starts `sqlnode` from environment variables; a process's id and name come from its hostname, as StatefulSet pods are named |
| `compose.yaml` | The cluster as 6 containers with Docker Compose (no Kubernetes needed) |
| `k8s/sqlengine.yaml` | Kubernetes: headless Services `coord` and `node`, StatefulSets with a 1 Gi volume per pod, Service `sql` (NodePort 30480) for clients, PodDisruptionBudgets, soft anti-affinity |
| `up.sh` | Build, load the image into minikube (or kind), apply, wait until ready |
| `chaos.sh` | Kill random pods (or containers) while `workload.sh` moves money between accounts; fails if the total changes |
| `workload.sh` | The money-moving client, run inside the image |

## Docker Compose

```bash
docker build -f deploy/Dockerfile -t sqlengine:dev .
docker compose -f deploy/compose.yaml up -d
build/src/sqlengine --connect 127.0.0.1:7481,127.0.0.1:7482,127.0.0.1:7483   # from the host
MODE=compose deploy/chaos.sh 60                                               # kill containers meanwhile
docker compose -f deploy/compose.yaml --profile client down -v                # -v: delete the data
```

## Kubernetes (minikube)

```bash
deploy/up.sh                       # minikube start (if needed), build, load, apply, wait
kubectl exec -it coord-0 -- sqlengine --connect coord-0.coord:7480,coord-1.coord:7480,coord-2.coord:7480
deploy/chaos.sh 120 15             # delete a random pod every 15 s for 2 minutes
kubectl delete -f deploy/k8s/sqlengine.yaml && kubectl delete pvc -l app.kubernetes.io/name=sqlengine
```

`minikube start --nodes 3` spreads the pods over three machines (the anti-affinity is a preference, so one machine works too). With kind: `KIND=1 deploy/up.sh`. CI runs both the compose and the kind deployments, with chaos, on every pull request (the `deploy` job).

Building behind a proxy that only tunnels HTTPS and re-signs TLS:
```bash
docker build --secret id=ca,src=/path/to/proxy-ca.crt \
  --build-arg UBUNTU_MIRROR=https://archive.ubuntu.com/ubuntu/ \
  --build-arg HTTPS_PROXY="$HTTPS_PROXY" -f deploy/Dockerfile -t sqlengine:dev .
```

## How it fits Kubernetes

- **Stable names:** pods find each other as `coord-1.coord` and `node-2.node`. The headless Services publish addresses before pods are ready (`publishNotReadyAddresses`), since a Raft group needs its peers to become ready at all. Both StatefulSets start their pods in parallel for the same reason.
- **A pod's identity is its name:** `coord-N` is coordinator `N + 1`, and `node-N` is storage node `node-N`. A replaced pod has a new IP but the same name and volume: it re-announces itself and catches up from its Raft log. This was tested by recreating containers on new IPs mid-workload.
- **Readiness:** a coordinator is ready when it answers SQL and knows the cluster's leader; a storage node is ready when it accepts calls. Service `sql` sends clients only to ready coordinators, and any of them forwards to the leader.
- **Vanished peers:** a killed pod's IP sends no TCP reset. Connections therefore time out on connect (2 s) and use TCP keepalive (a dead peer is noticed in about 5 s); outgoing connections also fail a send unacknowledged for 10 s. The coordinator also skips a node that just failed for a few seconds. Without this, one dead pod stalled the whole cluster for minutes.
- **Disruptions:** the PodDisruptionBudgets let a drain take one coordinator and one storage node at a time, so every Raft group keeps its quorum.
- **Not yet:** adding storage nodes after formation (`replicas: 4`) gives the new node no shards until rebalancing (D8), and pods are not spread by zone.
