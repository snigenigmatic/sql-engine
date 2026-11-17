# Distributed sql-engine — design and build plan

Status: D0–D5 are done; the rest is a proposal. It starts from `master` at 9d67d5b, the single-node engine with M1–M8 merged.

**Decisions already made**
- **Raft:** use the NuRaft library rather than writing our own.
- **Replication:** every shard is its own Raft group of 3 replicas. A separate metadata Raft group holds the cluster's metadata.
- **Coordinator:** 3 coordinator pods instead of one. The metadata group's leader plans and runs the queries.
- **Writes across shards:** atomic, using two-phase commit (2PC). Each participant in the commit is a Raft group, not a single node.

---

## 1. Functionalities
- **Sharded tables.** `CREATE TABLE t (...) SHARD BY HASH(col)`. The shard key defaults to the primary key.
- **Reference tables.** `CREATE TABLE t (...) REPLICATED` puts a full copy of a small table in every shard, so joins against it run locally.
- **Consistent hashing.** A ring of virtual nodes maps a key's hash to a shard. A second ring places each shard's 3 replicas on distinct nodes.
- **Replicated shards.** Each shard is one database file on each of 3 nodes. Writes go through Raft and commit once a quorum (2 of 3) has them. Losing any one node loses nothing and blocks nothing.
- **Cluster metadata.** Membership, the rings, shard placement, schema, statistics and transaction outcomes. It is held in the coordinators' own Raft group, and each change bumps a version number (the epoch).
- **Distributed SQL.** Everything the single-node engine supports works on the cluster, with the same results. The planner does:
  - shard pruning (sending a query only to the shards that can hold matching rows);
  - pushing filters, projections, partial aggregates and top-N down to the shards;
  - co-located and broadcast joins;
  - gathering everything else to the coordinator.
- **Distributed transactions.** `BEGIN … COMMIT` and multi-shard statements commit atomically through 2PC.
- **Operations:**
  - `SHOW CLUSTER`, `SHOW SHARDS` and `EXPLAIN` showing each query fragment;
  - adding and decommissioning nodes;
  - re-replicating a shard automatically after a node failure.
- **Deployment.** Docker image, minikube manifests, and a chaos test script.

## 2. Requirements

### Functional
- **Same answers:** every golden test in `test/sql/*.test` gives the same rows against the cluster as against a single node. A runner flag points the existing runner at the cluster.
- **Reads:** linearizable. They see every committed write. They are served by the shard leader under a leader lease, and never by a lagging follower.
- **Writes:** atomic and durable once acknowledged, at the level of the whole statement or transaction. They survive the loss of any one node.
- **DDL** (schema changes) is replicated to every shard group and to the metadata. A table never ends up existing on only some shards.

### Non-functional
- **Availability:** any single pod (coordinator or storage node) can die without loss of service.
  - **Failover:** a new Raft leader within about 2 s, with a 1 s election timeout. Clients retry transparently.
  - **Re-replication:** a node that stays dead for 5 minutes (configurable) gets its replicas rebuilt elsewhere from snapshots.
- **Consistency:** single-shard operations are linearizable. Multi-shard transactions are serializable, by locking whole shards for the duration of 2PC (coarse, but correct).
- **Determinism:** replicas apply the same statements in the same order and must reach the same state. The engine has no `RANDOM`/`NOW`, and any function added later must be deterministic or be evaluated at the coordinator.
- **Separation:** the single-node binary keeps working unchanged. Everything distributed lives in new modules.

### Scale (target)
- **Pods:** 3 coordinators and 3–5 storage nodes.
- **Shards:** 16 by default (configurable when the cluster is created), with replication factor 3. With 3 nodes that is about 16 replicas per node.
- **Size:** tables of up to millions of rows, and up to tens of concurrent clients. This is a teaching-scale system, not a production one.

## 3. Architecture

```
                 clients (sqlengine --connect host:port)
                                 │  TCP, length-prefixed frames
                                 ▼
              ┌──────── Service "sql" (NodePort) ────────┐
              ▼                    ▼                     ▼
        ┌──────────┐         ┌──────────┐          ┌──────────┐
        │ coord-0  │◄──Raft──►│ coord-1  │◄──Raft──►│ coord-2  │   metadata Raft group
        │ (leader) │         │(follower)│          │(follower)│   + query coordinator
        └────┬─────┘         └──────────┘          └──────────┘   (followers forward to leader)
             │ shard RPCs (fragments, prepare/commit)
   ┌─────────┼─────────────────────────────┐
   ▼         ▼                             ▼
┌───────────────┐  ┌───────────────┐  ┌───────────────┐
│ node-0        │  │ node-1        │  │ node-2        │   storage StatefulSet, one PVC each
│ s0(L) s1 s3.. │  │ s0  s1(L) s2..│  │ s0  s2(L) s3..│   sN = replica of shard N, (L) = leader
│ shard-N.db    │  │ shard-N.db    │  │ shard-N.db    │   each replica: own .db + Raft log
└───────────────┘  └───────────────┘  └───────────────┘
        ▲──────────── per-shard Raft groups (3 replicas each) ────────────▲
```

## 4. Components

### 4.1 `src/net`: transport and wire protocol
- **Transport:** plain POSIX TCP sockets with blocking I/O, one thread per connection (built in D1). Each frame is `u32 length | u8 type | payload`.
  - asio was the first idea, but blocking sockets suffice for request/response, `shutdown()` cleanly wakes a blocked thread when a server stops, and the client/server pieces then build without the NuRaft/asio download.
- **Message types:**
  - client side (D1): `EXECUTE` (one statement, or a script with or without stopping at the first error), `COMMAND` (`.tables`, `.schema`, `.save`), `RESULTS`, `TEXT`, `ERROR`;
  - shard side: `Fragment`, `RowBatch`, `Prepare`, `Vote`, `Decide`;
  - control: `Heartbeat`, `Join`, `NotLeader{hint}`.
- **Rows** are encoded with the existing `Tuple::SerializeTo`, so there is no new row format.
- **Streaming:** results come back in batches of about 1,000 rows with backpressure, so a large scan never has to be fully materialized on the shard. D1 still sends each result in one frame (up to 256 MiB); batching arrives with shard fragments in D5.

### 4.2 `src/server`: the `sqlnode` binary
- **Roles:** `--role=coordinator` or `--role=storage`.
- **Pod identity:** comes from the StatefulSet hostname, e.g. `node-1.node.default.svc`.
- **Client mode (D1):** `Shell` talks to a `Connection` (`src/session/connection.h`). `LocalConnection` wraps an in-process `Session`; `RemoteConnection` sends the same calls over TCP. `sqlengine --connect host:port` uses the latter. Dot commands still work; in the cluster, `.tables` and `.schema` will be answered from the metadata.
- **Single-node server (D1):** `sqlnode --database FILE --port N` serves one database. Clients take turns on it, and an open transaction keeps it to itself, so clients are serializable. The golden runner runs every `test/sql/*.test` file both in-process and through a server.

### 4.3 `src/cluster/ring`: consistent hashing
- **Hash:** 64-bit MurmurHash3 over the canonical key from `AppendGroupKey` (`src/execution/evaluator.h`). The canonical key is used so that `1` and `1.0` (and the NULL cases) hash the same way they compare.
- **Key → shard:** a ring of `shards × 256` virtual-node tokens. `ShardFor(hash)` finds the first token at or after the hash, wrapping around (`lower_bound` on a sorted vector). Measured over 200,000 integer and 200,000 text keys on 16 shards, rows per shard have a standard deviation of 7.8% and 8.3% of the mean.
- **Shard → nodes:** a second ring of nodes × 128 virtual nodes. A shard's replicas are the first 3 distinct nodes clockwise from `hash(shard_id)`, which is Dynamo's preference list.
  - Adding a node moves only about 1/N of the replicas, and moves no rows between shards.
- **Pure library (D2):** `src/cluster/ring` (`hash.h`, `hash_ring.h`, `placement.h`), with no I/O and no NuRaft dependency. Unit tests check:
  - the hash against reference MurmurHash3 outputs, and that `1`/`1.0`, `0.0`/`-0.0` and all NULLs hash alike;
  - even spread (stddev under 10% of the mean);
  - every process building the same ring whatever order nodes were added in;
  - a new node only joining replica sets, each losing at most one old member, about 1/N of them in all;
  - a removed node being replaced while every other member stays;
  - nodes holding between 0.6× and 1.4× their share of replicas.
  - Measured on 64 shards: 0.78–1.22× with 5 nodes and 0.83–1.29× with 8; adding a 5th node to 4 moved 43 replicas (about 38 expected).
- **Syntax (D2):** `CREATE TABLE … SHARD BY HASH(col)` and `CREATE TABLE … REPLICATED`. `SHARD`, `HASH` and `REPLICATED` are matched as words, not reserved, so columns may still have those names. The shard column must be one of the table's columns. A single node accepts the clause and ignores it; the metadata service stores it in D4.

### 4.4 `src/cluster/metadata`: the metadata state machine (on coordinators)
- **Contents:**
  - `nodes`: id, address, state (`up`/`suspect`/`down`/`decommissioning`), last heartbeat;
  - `ring`: tokens and the epoch;
  - `shards`: id, replica set, and the last known leader;
  - `tables`: schema, shard key or `REPLICATED`, and a schema version;
  - `stats`: per-shard results of `ANALYZE`;
  - `txns`: transaction id, participants, and `pending`/`committed`/`aborted`.
- **Storage:** an ordinary engine `Database` on each coordinator, i.e. the engine stores its own cluster metadata.
  - Commands are applied as SQL against tables named `__nodes`, `__shards` and so on, in the NuRaft `commit()` callback.
  - Each command bumps `epoch`.
- **Clients of the metadata:** storage nodes and follower coordinators cache it and subscribe to epoch changes. Any shard RPC carries the epoch. A node with an older epoch answers `StaleEpoch`, and the caller refreshes its cache and retries.

**As built in D4** (`src/cluster/node/coordinator.{h,cpp}`):
- **Storage:** the metadata group is a D3 `ReplicatedShard` whose members are the coordinators. Its tables are plain SQL tables (the cluster is the only client, so they don't need reserved names):
  - `cluster(shards, rf, epoch, formed)`;
  - `nodes(id, name, host, port, raft_base)`;
  - `replicas(shard, position, node)`: fixed when the cluster forms (rebalancing is D8);
  - `tables(name, definition, kind, shard_column)`, where `kind` is `hash`, `row` or `replicated`;
  - `ddl(seq, statement)`: every schema change, in order;
  - `shard_ddl(shard, applied)`: how far each shard has got through `ddl`.
- **Leadership:** the leader of the metadata group is the coordinator. On becoming leader it bumps `epoch`.
  - Every call to a storage node carries the epoch, and nodes refuse anything older than the newest they have seen. A deposed leader therefore cannot change data.
  - Followers forward clients' statements to the leader.
- **Formation:** storage nodes `join` (and keep re-announcing) until the leader accepts them. Once `rf` nodes have joined, the leader records `Placement`'s assignment in `replicas` and asks each replica's node to create its Raft group (`ensure_replica`). That request is idempotent and is repeated every 5 s, so a restarted node gets its replicas back.
- **Schema changes:** each DDL statement is first run against an empty copy of the schema, so errors match a single node. It is then appended to `ddl` and applied to every shard from where that shard left off.
  - A shard's progress is recorded after each statement, so after a crash at most the last statement repeats. Its harmless errors ("already exists", "not found") count as done.
  - Tables created before the cluster forms reach the shards when they form.
- **Distribution:** `SHARD BY HASH(col)`, else the primary key, else the hash of the whole row. `REPLICATED` tables get every write on every shard.
- **INSERT:** the statement first runs against an empty copy of its table. That gives single-node error messages and each row's final values (defaults, expressions), which are then hashed to pick shards.
- **UPDATE and DELETE** go to every shard, and the row counts are summed. Changing a hash table's shard key is refused; subqueries in them wait for D5.
- **SELECT and EXPLAIN:** the tables the statement names are gathered from the shards into an in-memory database with the full schema, which runs the statement. That is correct for every query but moves whole tables; D5 adds pruning and pushdown.
- **Known limits until later milestones:**
  - ~~a write that spans shards is not atomic~~ (D6: it is);
  - `UNIQUE` on a column other than the shard key is enforced per shard only;
  - ~~`BEGIN`/`COMMIT` are refused~~ (D6: distributed transactions, §4.7);
  - nodes that join after formation get no replicas (D8);
  - coordinator operations run one at a time.

### 4.5 `src/cluster/shard`: a replicated shard on NuRaft
- **One NuRaft `raft_server` per shard replica.**
  - Ports: `20000 + shard_id` on each pod. NuRaft listens per server, so a pod opens about 16 ports, which is fine at this scale.
  - Log store (D3, `FileLogStore`): one file, `<replica dir>/log`. It holds a header with the start index, then `u32 size | u32 crc32 | entry` records.
    - It is fsynced at the end of each append batch, which is before NuRaft acknowledges entries.
    - Any change of start index (compaction, or restarting at an index it doesn't hold) rewrites the file under a temporary name and renames it into place, so the start index and the entries can't disagree after a crash.
    - On open, a torn or corrupt tail is cut off.
  - Vote, term and membership: `FileStateManager` keeps them in `state` and `config`, each replaced atomically.
  - Bootstrap: every replica of a shard starts with the same member list and they elect a leader among themselves (`ReplicatedShard`).
- **State machine (`ShardStateMachine::commit`):** each log entry is one shard-local command.
  - `Exec{sql}` runs a single-shard write (D3). A `Barrier` entry applies nothing (see Reads).
  - `Prepare{txid, [sql…]}`, `Commit{txid}` and `Abort{txid}` carry 2PC.
  - `Ddl{sql, schema_version}` applies a schema change.
- **Statement-based replication:**
  - The coordinator rewrites each statement into shard-local SQL whose literals are fully bound, using the existing subquery binding in `BindOuterReferences` (`src/optimizer/optimizer.h`).
  - Each replica runs the statement through its own `Session` on `shard-N.db`, in log order. Because the engine is deterministic, all replicas reach the same state.
- **Exactly-once apply:** the last applied log index is stored in a `__raft_meta` table, committed in the same local transaction as the statement. After a restart, NuRaft replays from `last_applied + 1`.
  - `__raft_meta` is a *system table*, created with `Catalog::CreateTable(…, system=true)`. The executor refuses SQL that would change any table with a reserved `__` name (INSERT, UPDATE, DELETE, DROP, CREATE INDEX), so a client can't corrupt replication; it can still read them.
  - A statement that fails, fails identically on every replica; its index is recorded all the same. `BEGIN`/`COMMIT`/`ROLLBACK` sent to a shard are refused: transactions are log entries of their own (§4.7).
- **Idempotent writes (request ids):** a write that fails with "not committed" has an *unknown* outcome.
  - NuRaft cancels in-flight requests when leadership changes, yet the new leader may still commit them.
  - So every write carries a request id, and the replica records each id with its outcome in the `__raft_requests` system table, in the same local transaction as the write.
  - A retry with the same id returns the first outcome instead of applying again. The ledger keeps the last 10,000 log entries' ids, so retries must come within that window.
  - Found when a sanitizer CI run on a loaded runner had a write cancelled by a needless election.
- **Snapshots:**
  - Created with `Database::Checkpoint()` at exactly the snapshot's index, then copying `shard.db` to `snapshots/<index>.db` (plus NuRaft's metadata in `<index>.meta`).
  - Shipped in chunks (1 MiB by default) through NuRaft's snapshot API (`read_logical_snp_obj` / `save_logical_snp_obj`).
  - The first chunk carries the file's size and CRC32. The receiver installs only an intact copy; otherwise the install fails and the leader sends it again. This was added after a test showed that a truncated transfer crashed the replica on open.
  - Used for catching up a new or wiped replica and for compacting the log.
- **Reads (D3):** they run on the leader only, after a **read barrier**. The leader commits a no-op `Barrier` entry through the log, then reads its own data.
  - The barrier committing proves the leader still led after the read arrived, so the read sees every earlier commit (linearizable). It is the idea behind Raft's ReadIndex, and needs no clocks.
  - A leader lease (NuRaft's `leadership_expiry`) was the first idea. But NuRaft only checks it on its heartbeat timer, which bounds staleness by timing rather than ruling it out.
  - A leader cut off from its quorum refuses reads.
  - The barrier costs one replication round per read. A lease could be added later as an optimization for reads that can tolerate the timing assumption.
  - The engine allows one connection per file. So applying log entries and serving reads share one `Session`, taking turns under a mutex; reads never see half-applied state.
  - A non-leader answers `NotLeader{hint}`.

### 4.6 `src/distributed/planner`: the distributed planner (on the coordinator leader)
- **Starting point:** reuse the existing parser, scope resolution (`Scope`) and estimator (`Estimator`). The distributed planner splits the logical plan into **fragments**.
  - **Shard pruning:** `key = lit` or `key IN (…)` → only the matching shards. Otherwise → all of them.
  - **Pushdown:** filters, projections and `LIMIT` run on each shard. `ORDER BY … LIMIT n` → a top-n per shard, then a merge.
  - **Partial aggregation:**
    - `COUNT`/`SUM`/`MIN`/`MAX` → computed per shard, then combined.
    - `AVG` → `SUM` and `COUNT` per shard.
    - `COUNT(DISTINCT)` and `DISTINCT` → grouped per shard, then deduplicated at the coordinator.
  - **Joins:**
    - **Co-located:** both tables are sharded on the join key → the whole join is pushed to every shard.
    - **Broadcast:** the other side is a `REPLICATED` table → also pushed down.
    - **Otherwise:** gather the smaller side, picked using the summed per-shard `ANALYZE` statistics.
  - **Subqueries:** uncorrelated ones are run first and bound as literals. Correlated ones → gather.
- **Coordinator execution:** fragment results stream into temporary tables in an in-memory `Database` (`Database::OpenInMemory`). The rest of the query (merge, final aggregate, joins, sort) then runs locally through the **existing** executor, so no operator has to be rewritten.
  - Large results are a risk because the engine only sorts in memory. Mitigation: `LIMIT` pushdown and streaming merges.
- **`EXPLAIN`** prints the fragments, which shards each one goes to, and the coordinator's local plan.

**As built in D5** (`Coordinator::PlanSelect`): every `SELECT` still ends with the coordinator running the statement itself over the rows it fetched, so pushing work to shards can only shrink the data moved, never change the answer. In order of preference:
1. **The whole statement on one shard.** Every table the statement names is either `REPLICATED` or a hash table whose own `WHERE` conditions pin its key to one shard, the same one for all of them (`key = literal`, `key IN (literals)`). The shard's answer is the answer. This covers key lookups and joins of rows sharing a key or with replicated tables.
2. **Partial aggregates.** One table, no `GROUP BY`/`HAVING`/`ORDER BY`/`LIMIT`/`DISTINCT`, and only `COUNT`/`SUM`/`MIN`/`MAX`/`AVG` (not `DISTINCT`) in the select list.
   - Each shard returns its partials, with `AVG` sent as `SUM` plus `COUNT`.
   - The coordinator combines them with the engine's own rules: a 64-bit integer sum, float sums kept apart, and "Integer overflow" outside 32 bits.
   - Column names and type checks come from running the statement on the empty schema.
3. **Gather.** Each table is fetched from the shards its conditions allow, with the `WHERE` conjuncts that mention only that table applied on the shards.
   - Not pushed for the NULL side of a `LEFT JOIN`, nor for a table named more than once (self-join, or also inside a subquery), since those need the rows the conditions would drop.
   - Single-table `ORDER BY col… LIMIT n OFFSET m` also sends `ORDER BY … LIMIT n+m` to the shards (plain table columns only, not positions or aliases).
   - The coordinator then runs the statement over what it fetched.
- **`EXPLAIN`** on a cluster starts with the distributed plan: the strategy, the shards and each query sent, followed by the shard's or the coordinator's own plan.
- **`UPDATE`/`DELETE` with subqueries:** each uncorrelated subquery runs once on the cluster beforehand and is replaced by literals.
  - A scalar subquery becomes its value, or `NULL` when it returns no rows, with the single-node error if there are several rows.
  - `EXISTS` becomes `TRUE`/`FALSE`.
  - `IN (SELECT …)` becomes an `IN` list, and over no rows `FALSE` (`NOT IN`: `TRUE`).
  - The statement then goes to every shard. Subqueries that refer to the row being changed are refused.
- **The golden SQL files also run on an in-process cluster** (`sql_logic_test`, `Cluster` mode) and give the single-node rows; since D6 that includes `transactions.test`.
- **Not done (yet):**
  - projection pushdown;
  - `GROUP BY` pushdown (grouping by the shard key could run per shard);
  - shuffle joins, and joins that are co-located on the key but not pinned to one shard;
  - float sums can differ from a single node in the last digits, because the cluster adds in a different order;
  - a single shard's partial integer `SUM` can overflow even when the total would not.

### 4.7 Two-phase commit over Raft groups

**As built (D6).** It differs from the plan below where noted; the code is in `ShardStateMachine` (the participant) and `Coordinator` (the coordinator).
- **Interactive, not buffered:** each statement of a transaction runs on its shards at once, as a `Txn{exec, txid, sql}` log entry. The replica keeps its own database transaction open across entries, so the transaction **reads its own writes** (its reads carry the txid), and statements fail at once with single-node errors (`UNIQUE`, `NOT NULL`, …).
  - A statement that fails on one shard is undone on the others (`Txn{undo}` rolls back to a savepoint taken before the statement), so statements stay atomic inside a transaction and the transaction stays open, as on a single node.
- **Shard locks:** while a transaction is open on a shard, every other write and every read outside that transaction is refused with "locked by another transaction". The coordinator undoes the statement, waits (backing off, with the operation lock released) and retries, up to `lock_timeout_ms` (5 s). A timed-out statement fails; an explicit transaction stays open, and the client decides. Two transactions waiting on each other both time out (no deadlock detection yet).
  - Reads in a transaction lock nothing, so isolation is *read committed* for shards the transaction has not written, and serializable for those it has.
  - An `UPDATE`/`DELETE` that changed no row on a shard lets go of that shard at once, so a transaction only locks the shards it really wrote.
- **Statements outside `BEGIN`:** one shard → a plain write. More than one (multi-row `INSERT`, `UPDATE`, `DELETE`, DDL, `ANALYZE`) → an implicit transaction committed with 2PC, so **every statement is atomic across shards**.
- **DDL** runs on every shard inside the transaction; the metadata (`tables`, `ddl`, `shard_ddl`) changes with the commit decision. Inside `BEGIN`, a new table is visible to the transaction only, and `ROLLBACK` takes it away.
- **COMMIT:** one participant → `Txn{commit}` is the decision. Several → `Txn{prepare}` on each (a prepared shard takes no more statements), then the decision is one all-or-nothing metadata batch (`INSERT INTO txns (id, 'committed')` plus any schema changes), then `Txn{commit}` on each, then the `txns` row is deleted.
- **Recovery (presumed abort):** a new leader asks every shard which transaction it has open (`txn_status`). One that no client session owns is looked up in `txns`: committed → commit it; otherwise it inserts `(id, 'aborted')` first (the primary key makes a late commit from the old leader lose) and aborts it. Shards a leader could not reach or settle are retried every second. A client that disconnects mid-transaction is rolled back at once.
- **Replica restarts:** an open transaction is never committed locally, so `__raft_meta` still points before it. Replaying the log reopens it, prepared or not, exactly as it was. Snapshots are refused while a transaction is open (the database file would not contain it).

**The original plan:**
- **Statements:** an auto-commit statement that touches 1 shard → a single `Exec` entry, with no 2PC.
- **Transactions:** in `BEGIN … COMMIT`, the coordinator buffers each shard's rewritten writes.
  - Reads inside the transaction run against committed data.
  - Limitation for v1: a transaction does not read its own uncommitted writes. A read after a write to the same table inside a transaction is rejected with a clear error.
- **COMMIT:**
  1. Send `Prepare{txid, statements}` to every participant shard's leader. It is appended to that shard's Raft log, and every replica runs it inside a local `BEGIN`, which is left open.
  2. While a shard is prepared it is locked: any other write to it waits, and its reads wait.
  3. Each participant votes YES if all its statements succeeded (constraints included). Otherwise it votes NO and its local transaction is rolled back.
  4. The coordinator writes `txns[txid] = committed | aborted` to the **metadata Raft group**. This is the commit point.
  5. It then sends `Commit` or `Abort` to every participant. Each one commits or rolls back its local transaction and releases its shard lock.
- **Deadlocks:** no-wait. If a shard is already prepared for another transaction, the new one gets a NO vote, aborts, and retries with backoff, up to 5 times.
- **Recovery:**
  - **Participant restart:** replaying the Raft log re-runs `Prepare` and restores the prepared state. The participant then asks the metadata group for the outcome.
  - **Coordinator death:** a new metadata leader scans `pending` transactions. Anything not committed is aborted (presumed abort), and the decisions are re-sent.
  - **Why it can't hang:** both the participants and the decision live in Raft groups, so a single dead node cannot leave a transaction stuck.

### 4.8 `src/cluster/membership`: membership and rebalancing (on the coordinator leader)
- **Join:** a node starts → `Join` → it is added to `nodes` and the node ring → the shards whose preference lists now include it gain a replica.
  - The new replica is added as a NuRaft learner, catches up from a snapshot, is promoted to voter, and then the old replica is removed.
  - Only one membership change happens per shard at a time.
- **Failure detection:** heartbeats every 1 s.
  - 5 missed → `suspect`.
  - 5 min → `down` → its replicas are re-placed using the ring.
  - Raft leadership has already failed over long before this; re-placement only restores the replication factor.
- **Decommission:** `ALTER CLUSTER REMOVE NODE n` drains its replicas, then removes the node.
- **Stretch goals:**
  - splitting a shard by adding tokens, which moves one hash range to a new shard group;
  - shuffle joins;
  - reads from followers at a bounded staleness.

### 4.9 `deploy/`: Kubernetes on minikube
- **Image:** a multi-stage `Dockerfile` (build with CMake, then a slim runtime image holding just `sqlnode`).
- **Coordinators:** `StatefulSet coord`, 3 replicas, with a headless Service `coord`.
- **Storage nodes:** `StatefulSet node`, 3 replicas, a 1 Gi PVC each, and a headless Service `node`.
- **Client access:** `Service sql`, a NodePort pointing at the coordinators.
- **Probes:**
  - readiness: the process has joined and its Raft groups have a leader;
  - liveness: TCP check.
- **Scheduling:** a `PodDisruptionBudget` with `maxUnavailable: 1`, plus pod anti-affinity.
  - Use `minikube start --nodes 3` to spread pods over separate VMs; on one minikube node, process-level failover still works.
- **Scripts:**
  - `deploy/up.sh` brings the cluster up;
  - `deploy/chaos.sh` loops `kubectl delete pod` and partitions pods with NetworkPolicy, while a test client runs.

## 5. Request flows

**A query that touches one shard:** `SELECT * FROM orders WHERE id = 42`
```
client → coord leader: Query
coord: parse → plan → prune: hash(42) → shard 5 → leader cached = node-1
coord → node-1/s5: Fragment(SELECT … WHERE id = 42, epoch 17)
node-1: lease ok, run under shard mutex → RowBatch… → done
coord → client: Result
```

**A write across shards:** `UPDATE accounts SET bal = bal - 10 WHERE id IN (1, 7)`, where the two ids live on shards 2 and 9
```
coord: txid=88, rewrite per shard
coord → s2 leader, s9 leader: Prepare(88, [UPDATE … WHERE id = 1]) / (… id = 7)
s2, s9: append to own Raft log → quorum → every replica runs it in open local txn → Vote YES
coord → metadata Raft: txns[88] = committed          ◄── commit point
coord → s2, s9: Commit(88) → Raft → replicas COMMIT local txn, release shard lock
coord → client: 2 row(s) updated
```

## 6. Failure handling

| Failure | What happens | Client sees |
|---|---|---|
| A storage pod dies | Each shard it led elects a new leader among the 2 survivors (≈1–2 s). Coordinators get `NotLeader` or a timeout, refresh the leader cache, and retry. | A short pause, no errors |
| It stays dead for over 5 min | Its replicas are re-placed on other nodes from snapshots | Nothing |
| The coordinator leader dies | The metadata group elects a new leader. It reloads its state and resolves `pending` transactions (presumed abort). Clients reconnect through the Service. | One failed statement if it was mid-flight; the client retries |
| Death between Prepare and the decision | Participants stay prepared (shard locked) until the new leader decides | That transaction aborts and can be retried |
| A network partition | The minority side cannot form a quorum: reads stop once the lease expires, and writes stop | Errors on the minority side only |
| Two nodes of one shard lost | That shard loses quorum and becomes read-unavailable; other shards keep working | Errors only for keys on that shard |
| A disk is corrupt or a replica falls behind | It is rebuilt from a snapshot taken from the leader | Nothing |

## 7. Build order

Each milestone ends green in CI (Debug, ASan/UBSan, Release) and is one PR.

| # | Milestone | Done when |
|---|---|---|
| D0 ✅ | **Dependencies and layout.** Pull NuRaft and standalone asio in via CMake `FetchContent`, add the `src/cluster` module (`src/net` and `src/distributed` arrive with their first code in D1 and D5) | Builds on CI, single-node tests unchanged |
| D1 ✅ | **Server mode.** The wire protocol, a single-node `sqlnode` with one `Session` per connection (serialized), and the CLI's `--connect` | The golden runner passes against a server over TCP |
| D2 ✅ | **Rings and placement.** The `ring` library, the metadata model, and the `SHARD BY` / `REPLICATED` syntax | Unit tests for spread, minimal movement and placement |
| D3 ✅ | **One replicated shard.** The NuRaft state machine on `Database`, the log store, snapshots and exactly-once apply | An in-process 3-replica test: kill the leader, keep writing, restart, catch up, install a snapshot, same `.db` contents on every replica |
| D4 ✅ | **The metadata group and cluster.** The coordinators' Raft group, node join, shard bootstrap, DDL fan-out, epochs | An in-process cluster of 3 coordinators and 3 nodes boots, then `CREATE TABLE`/`INSERT`/`SELECT` work |
| D5 ✅ | **Distributed queries.** Pruning, scatter-gather, pushdown, partial aggregates, co-located and broadcast joins, gathering, distributed `EXPLAIN` | Every `test/sql/*.test` gives the same rows on the cluster; `EXPLAIN` tests show pruning and pushdown |
| D6 ✅ | **2PC.** Prepare, decide and commit over Raft, the transaction table, recovery, shard locks with no-wait retry | A bank-transfer test keeps total money constant while leaders and coordinators are killed |
| D7 | **Kubernetes.** The `Dockerfile`, the manifests, `up.sh`/`chaos.sh`, probes | On minikube: golden tests pass, and pass again under the `chaos.sh` loop |
| D8 | **Rebalancing.** Join, decommission, re-replication after `down`; `SHOW CLUSTER`/`SHOW SHARDS` | Adding a 4th node moves about 1/4 of the replicas with no errors, and killing a node permanently restores the replication factor to 3 |

## 8. Testing strategy
- **Unit tests:** the rings, placement, fragment planning (EXPLAIN text), the transaction state machine, and frame encoding.
- **In-process cluster harness:** `test/cluster/` starts coordinators and nodes as threads on localhost ports, with a fault injector that can kill, restart or partition (by dropping frames) individual servers. It is deterministic enough to run in CI.
- **Same answers:** `sql_logic_test --cluster` reuses every golden file, checked against single-node results.
- **Invariant tests:**
  - **bank:** concurrent transfers, total money must never change;
  - **register:** single-key read and write histories checked for linearizability with a small Knossos-style checker.
- **Mutation checks:**
  - drop the lease check → the stale-read test must fail;
  - skip writing the decision → the recovery test must fail;
  - apply a log entry twice → the exactly-once test must fail.
- **On minikube:** `chaos.sh` runs for a set number of minutes as a manual or nightly job. It is not part of PR CI.

## 9. Risks and open points
- **NuRaft fit:** settled in D0.
  - NuRaft 2.1.0 is built from its sources by our own CMake target (`cmake/NuRaft.cmake`), because its CMakeLists always builds its examples and tests and forces C++11.
  - asio is pinned to 1.28: NuRaft still uses `io_service`, which asio removed in 1.33, and the 1.12 that NuRaft pins does not build with GCC 13.
  - NuRaft is linked against OpenSSL even though TLS stays off: its no-SSL build uses a mock socket that newer asio rejects.
  - Its headers miss `<cstdint>`, so our code includes NuRaft through `src/cluster/raft/nuraft.h`.
  - `test/cluster/nuraft_test` runs real servers on localhost. A 3-replica group replicates a log, elects a new leader when the old one stops, and stops committing without a quorum. Two groups of 3 share one process, one port each, which is how a storage pod will hold many shards.
  - Each `raft_launcher` brings its own asio thread pool. For ~16 shards per pod, D3 should share one `asio_service` between a pod's Raft servers.
- **Write serialization:** a shard's single connection serializes all its writes, and shard-level 2PC locks are coarse. Fine at this scale; row-level locking is future work.
- **Read-your-writes in transactions:** not supported in v1, as described in 4.7.
- **Large gathers:** results are bounded by coordinator memory, because sorting is in-memory only. Spilling to disk is future work.
- **Schema changes vs. in-flight queries:** handled by schema versions on fragments, which are rejected and retried on a mismatch.
