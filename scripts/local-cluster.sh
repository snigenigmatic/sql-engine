#!/bin/bash
# A cluster on this machine: 3 coordinators and 3 storage nodes, each its
# own sqlnode process (like pods on a mini cluster, without containers).
#
#   scripts/local-cluster.sh up            start all six (data in $CLUSTER_DIR)
#   scripts/local-cluster.sh ps            which are running
#   scripts/local-cluster.sh kill NAME     kill -9 one: coord-1..3, node-0..2
#   scripts/local-cluster.sh start NAME    start one again (its data is kept)
#   scripts/local-cluster.sh down          kill -9 all (data kept)
#   scripts/local-cluster.sh wipe          down, then delete the data
#
# Clients: build/src/sqlengine --connect 127.0.0.1:7481,127.0.0.1:7482,127.0.0.1:7483
# Environment: SQLNODE (default build/src/server/sqlnode), CLUSTER_DIR
# (default /tmp/sqlengine-cluster), SHARDS (default 8).
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=${SQLNODE:-build/src/server/sqlnode}
ROOT=${CLUSTER_DIR:-/tmp/sqlengine-cluster}
SHARDS=${SHARDS:-8}
COORDS="1=127.0.0.1:7101:7481,2=127.0.0.1:7102:7482,3=127.0.0.1:7103:7483"

start() {
  mkdir -p "$ROOT/logs" "$ROOT/pids"
  case "$1" in
    coord-[123])
      local id=${1#coord-}
      nohup "$BIN" --role coordinator --id "$id" --coordinators "$COORDS" --dir "$ROOT/data/$1" \
        --shards "$SHARDS" --rf 3 > "$ROOT/logs/$1.log" 2>&1 &
      ;;
    node-[012])
      local n=${1#node-}
      nohup "$BIN" --role storage --name "$1" --coordinators "$COORDS" --dir "$ROOT/data/$1" \
        --raft-base $((8000 + 100 * n)) --host 127.0.0.1 --port $((7501 + n)) > "$ROOT/logs/$1.log" 2>&1 &
      ;;
    *) echo "unknown process $1" >&2; exit 1 ;;
  esac
  echo $! > "$ROOT/pids/$1"
}

stop() {
  local pidfile="$ROOT/pids/$1"
  [ -f "$pidfile" ] && kill -9 "$(cat "$pidfile")" 2> /dev/null || true
  rm -f "$pidfile"
}

ALL="coord-1 coord-2 coord-3 node-0 node-1 node-2"
case "${1:-}" in
  up) for p in $ALL; do start "$p"; done ;;
  ps) for p in $ALL; do
        if [ -f "$ROOT/pids/$p" ] && kill -0 "$(cat "$ROOT/pids/$p")" 2> /dev/null; then echo "$p up"; else echo "$p down"; fi
      done ;;
  kill) stop "$2" ;;
  start) start "$2" ;;
  down) for p in $ALL; do stop "$p"; done ;;
  wipe) for p in $ALL; do stop "$p"; done; rm -rf "$ROOT" ;;
  *) sed -n '2,15p' "$0"; exit 1 ;;
esac
