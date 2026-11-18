# Testing the cluster as its users would (D6)

After D6 (two-phase commit), the cluster was run as six separate `sqlnode` processes on one machine (3 coordinators, 3 storage nodes, 8 shards × 3 replicas, Release build) and used the way people would use it: through the `sqlengine` CLI in real terminals (pseudo-terminals driven by a script), several at once, while processes were killed with `kill -9`.

To reproduce:
```bash
scripts/local-cluster.sh up
scripts/cluster_users.py setup      # then: clerks, deadlock, chaos
build/src/sqlengine --connect 127.0.0.1:7481,127.0.0.1:7482,127.0.0.1:7483   # to look around yourself
scripts/local-cluster.sh wipe
```

## What was tried, and what happened

| Who | What they did | Result |
|---|---|---|
| App developer | Created a shop schema (`customers` by id, `products` REPLICATED, `orders` SHARD BY customer), loaded 4,040 rows with `.read` (3.8 s), ran joins, aggregates, `IN (SELECT …)`, `LEFT JOIN … GROUP BY`, `EXPLAIN` | Output **byte-identical** to the same data in a single-node database (8 queries, floating-point averages included); `EXPLAIN` shows a one-shard plan for `customer_id = 42` |
| Two clerks | Both update customer 7: one inside `BEGIN`, the other at the same time | The second waits (1.6 s) until the first commits, then applies on top (1000 → 700 → 750). Work on other customers does not wait |
| A forgetful user | `BEGIN; UPDATE …` and walks away; then the laptop dies (CLI killed) | Others touching those shards wait, then get "lock wait timed out" after 5 s. When the client dies, its transaction is rolled back at once and everything proceeds |
| Two users, opposite transfers | A: 7 → 8, B: 8 → 7, interleaved | "Deadlock detected: the transaction was rolled back; retry it" for one at once; the other commits; balances consistent |
| Six users, operations | Random transfers in `BEGIN … COMMIT` for 36 s while a storage node and then the leading coordinator are `kill -9`ed and restarted (`scripts/cluster_users.py chaos`) | **2,032 transfers committed, total balance unchanged** (1,000,000 before and after; an earlier run: 2,104, also unchanged). Throughput held through both failures (≈380–600 commits per 8–10 s phase). 71 failures, all reported clearly: 64 deadlocks, 7 transactions caught on the killed coordinator |
| Operations, afterwards | Stopped everything and opened every replica's `shard.db` directly | All 3 replicas of all 8 shards identical, including the restarted node's; per-shard sums add up to the cluster total |

## Problems found, and fixed in D6

1. **Deadlocks stalled for the whole lock timeout.** Two transactions that each held a shard the other wanted both waited 5 s; with many users, throughput fell to a few transactions per second. *Fix:* the coordinator (which sees every transaction) keeps a waits-for graph and rolls back the transaction that closes a cycle, at once.
2. **A killed coordinator stranded its clients.** The CLI kept saying "not connected" forever, even after the coordinator came back. *Fix:* `--connect` takes every coordinator (`h1:p1,h2:p2,h3:p3`), and a lost connection is re-established at the next statement, to whichever answers. A transaction open when the connection was lost is reported as rolled back, and the statement is not run on its own.
3. **Clients of a follower failed during an election** ("no coordinator is leading right now") instead of waiting the second it takes. *Fix:* followers wait for the new leader, and never forward a transaction's later statements to a different leader than its earlier ones.
4. **Every `UPDATE`/`DELETE` locked every shard**, so any open transaction blocked everyone's updates. *Fix:* they go only to the shards their `WHERE key = …` / `key IN (…)` can match, and a shard where the statement changed nothing is released at once.
5. **A forgotten `BEGIN` held its shards until the client disconnected.** *Fix:* a transaction idle for 60 s (`sqlnode --txn-idle-timeout`) is rolled back; its client's next statement is told so and not run.

## Known limits (not fixed)

- **No MVCC.** A shard written by an open transaction is locked to everyone else, readers included, until it commits. Long transactions block reports on the same shards (for up to `--lock-timeout`, then an error).
- **Lock granularity is the shard,** not the row: two transactions on different rows of the same shard wait for each other. More shards mean fewer conflicts.
- Deadlock victims (and timed-out statements) must be retried by the application.
- Reads inside a transaction see committed data of shards the transaction has not written (*read committed* there).
- `.tables` on a cluster shows columns but not row counts.
