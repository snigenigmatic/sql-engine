#!/bin/sh
# Money moving between accounts, through the sqlengine CLI. Every transfer is
# one statement that changes two rows (usually on two shards), so the
# cluster must apply it all or nothing: the total never changes, whatever
# fails meanwhile.
#
#   workload.sh setup [ACCOUNTS]   accounts 1..ACCOUNTS (default 100) with 1000 each
#   workload.sh transfers SECONDS  transfer between random accounts until then
#   workload.sh total              COUNT and SUM of the balances
#
# SQL_ADDRESSES: the coordinators (default: the Kubernetes StatefulSet's)
set -eu
addresses=${SQL_ADDRESSES:-coord-0.coord:7480,coord-1.coord:7480,coord-2.coord:7480}
sql() { sqlengine --connect "$addresses"; }

case "${1:-}" in
  setup)
    accounts=${2:-100}
    # Wait (up to 3 minutes) for a leader and a formed cluster
    tries=0
    until printf '.help\n' | sql 2> /dev/null | grep '^Cluster: ' | grep -qv forming; do
      tries=$((tries + 1))
      if [ "$tries" -gt 90 ]; then echo "workload: the cluster did not form" >&2; exit 1; fi
      sleep 2
    done
    {
      echo "CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER NOT NULL);"
      awk -v n="$accounts" 'BEGIN { printf "INSERT INTO accounts VALUES "
        for (i = 1; i <= n; i++) printf "%s(%d, 1000)", (i > 1 ? ", " : ""), i; print ";" }'
    } | sql
    ;;
  transfers)
    end=$(( $(date +%s) + ${2:-60} ))
    accounts=$(echo "SELECT COUNT(*) FROM accounts;" | sql | awk '/^ *[0-9]+ *$/ { print $1 }')
    ok=0 failed=0 seed=$$
    while [ "$(date +%s)" -lt "$end" ]; do
      seed=$((seed + 1))
      # 20 transfers: id a pays x to id b; (id - a) / (b - a) is 0 for a, 1 for b
      out=$(awk -v n="$accounts" -v seed="$seed" 'BEGIN { srand(seed)
        for (i = 0; i < 20; i++) {
          a = 1 + int(rand() * n); b = 1 + int(rand() * n); x = 1 + int(rand() * 50)
          if (a == b) b = a % n + 1
          printf "UPDATE accounts SET balance = balance + %d * (2 * ((id - %d) / (%d - %d)) - 1) WHERE id IN (%d, %d);\n", x, a, b, a, a, b
        } }' | sql 2>&1 || true)
      done_now=$(printf '%s\n' "$out" | grep -c '^2 row(s) updated' || true)
      ok=$((ok + done_now))
      failed=$((failed + 20 - done_now))
    done
    echo "transfers: $ok applied, $failed failed"
    ;;
  total)
    echo "SELECT COUNT(*), SUM(balance) FROM accounts;" | sql | awk -F'|' '/^ *[0-9]+ *\|/ { gsub(/ /, ""); print $1 "|" $2 }'
    ;;
  *)
    sed -n '2,13p' "$0"
    exit 2
    ;;
esac
