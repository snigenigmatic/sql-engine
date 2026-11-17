#pragma once

#include "catalog/database.h"
#include "parser/ast.h"
#include "cluster/shard/replicated_shard.h"
#include "net/remote_connection.h"
#include "server/server.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace sql::cluster
{

    // A coordinator. The coordinators form a Raft group (a ReplicatedShard)
    // whose database holds the cluster's metadata:
    //
    //   cluster   (shards, rf, epoch, formed)
    //   nodes     (id, name, host, port, raft_base)     storage nodes
    //   replicas  (shard, node)                          fixed at formation
    //   tables    (name, definition, kind, shard_column) kind: hash|row|replicated
    //   ddl       (seq, statement)                       every schema change, in order
    //   shard_ddl (shard, applied)                       how far each shard got
    //   txns      (id, state)                            2PC decisions not yet
    //                                                    applied everywhere
    //
    // The group's leader is the cluster's coordinator: it accepts storage
    // nodes, forms the cluster once `replication_factor` nodes have joined
    // (placing every shard's replicas with Placement and creating their
    // Raft groups), keeps every shard's schema up to date from the ddl log,
    // and runs clients' SQL:
    //
    //   DDL               validated, appended to ddl, applied to every shard
    //   INSERT            each row to its shard (by key hash; replicated
    //                     tables to every shard)
    //   UPDATE / DELETE   to every shard; row counts are summed
    //   SELECT / EXPLAIN  the tables it names are gathered from the shards
    //                     into an in-memory database, which runs it (D5 adds
    //                     pruning and pushdown)
    //
    // Becoming leader bumps the epoch, which storage nodes use to refuse a
    // deposed leader. Followers redirect clients to the leader.
    //
    // Writes are atomic across shards (D6). A statement that writes one
    // shard is a plain write; anything wider, and everything inside BEGIN
    // ... COMMIT, runs as a distributed transaction: each shard keeps the
    // transaction open in its Raft log (and is locked to everyone else
    // meanwhile), and COMMIT is two-phase: every participant prepares, the
    // decision is written to the metadata group (the commit point), then
    // every participant commits. A new leader finishes or rolls back
    // whatever an earlier one left open (presumed abort). Waiting for a
    // locked shard gives up after `lock_timeout_ms`.
    class Coordinator
    {
    public:
        struct Options
        {
            int id = 1;
            std::string host = "127.0.0.1";
            uint16_t client_port = 0; // 0: any free port
            int raft_port = 0;
            std::string directory;
            std::vector<ReplicatedShard::Member> peers;    // every coordinator's Raft endpoint
            std::map<int, std::string> client_addresses;  // coordinator id -> host:client_port
            uint32_t shard_count = 16;
            size_t replication_factor = 3;
            int heartbeat_ms = 100;
            int election_timeout_ms = 1000;
            int snapshot_distance = 1000;
            int lock_timeout_ms = 5000; // waiting for another transaction's shard
            // A transaction no statement used for this long is rolled back,
            // releasing its shards (0: never)
            int idle_transaction_timeout_ms = 60000;
        };

        // A client's transaction (one per client connection)
        struct Transaction
        {
            std::string id; // empty: none open
            uint64_t epoch = 0;
            std::set<uint32_t> shards;    // participants
            std::vector<std::string> ddl; // schema changes, applied to the metadata at COMMIT
            bool implicit = false;        // one statement's own, outside BEGIN
            std::chrono::steady_clock::time_point last_used{};
            // Rolled back by the coordinator (idle too long): the client's
            // next statement is told so instead of running on its own
            std::string expired;
            bool Open() const { return !id.empty(); }
        };

        explicit Coordinator(const Options &options);
        ~Coordinator();

        void Start();
        void Stop();
        uint16_t Port() const { return server_ ? server_->Port() : 0; }
        bool IsLeader() const { return metadata_ && metadata_->IsLeader(); }
        // How long a follower waits for a new leader before failing a client
        int ElectionWaitMs() const { return options_.election_timeout_ms * 5; }
        // host:client_port of the current leader, or "" if unknown
        std::string LeaderAddress() const;

        // The client-facing operations (on the leader). `txn` is the
        // client's transaction state; without one, BEGIN is refused.
        ExecutionResult Execute(const std::string &sql, Transaction *txn = nullptr);
        // The client went away: roll back its transaction, if any
        void Abandon(Transaction *txn);
        // Whether the client is (as far as it knows) inside a transaction
        bool InTransaction(const Transaction *txn);
        // Forget the client's transaction (this coordinator lost the
        // leadership: the next leader rolls it back); true if one was open
        bool Drop(Transaction *txn);
        std::string Command(const std::string &name, const std::string &argument);
        ExecutionResult Call(const std::string &method, const std::vector<std::string> &arguments);

        // For tests: shard reads sent so far
        uint64_t ShardReads() const { return shard_reads_; }
        // For tests: the metadata group's replica here
        ReplicatedShard &Metadata() { return *metadata_; }
        // For tests: distributed transactions committed with two phases
        uint64_t TwoPhaseCommits() const { return two_phase_commits_; }

        // For tests and status: as last loaded by the leader
        bool Formed();
        size_t NodeCount();
        uint64_t Epoch();

    private:
        struct NodeInfo
        {
            int id = 0;
            std::string name, host;
            uint16_t port = 0;
            int raft_base = 0;
        };
        struct TableInfo
        {
            std::string name, definition, kind, shard_column;
        };
        struct View
        {
            uint32_t shards = 0;
            size_t rf = 0;
            uint64_t epoch = 0;
            bool formed = false;
            std::map<int, NodeInfo> nodes;
            std::map<uint32_t, std::vector<int>> replicas;
            std::map<std::string, TableInfo> tables;
            std::vector<std::pair<int64_t, std::string>> ddl;
            std::map<uint32_t, int64_t> shard_ddl;
        };

        // All below: caller holds op_mutex_, on the leader
        void Loop();
        void BecomeLeader();
        void Reconcile();
        void Load();
        // batch: several statements, all or nothing
        ExecutionResult MetaWrite(const std::string &sql, bool batch = false);
        ExecutionResult MetaRead(const std::string &sql);
        void Form();
        void EnsureReplicas(bool force);
        void CatchUpDdl();
        // A call to a shard's leader: "write" (sql), "read" (sql, inside
        // txn_ if open), "txn" (op, txid, sql) or "txn_status". Writes and
        // transaction steps carry a request id, kept across retries.
        ExecutionResult ShardCall(uint32_t shard, const std::string &method, const std::string &sql,
                                  const std::string &op = "", const std::string &txid = "", int timeout_ms = 0);
        ExecutionResult ExecuteLocked(const std::string &sql, Transaction *txn);
        ExecutionResult RunTransactionStatement(const TransactionStatement &stmt, Transaction *txn);
        // Runs body inside txn_, or, outside a transaction, inside an implicit
        // one that commits if body succeeds and rolls back if not
        ExecutionResult Atomically(const std::function<ExecutionResult()> &body);
        // Runs per-shard statements: one shard outside a transaction is a
        // plain write; otherwise inside txn_ (callers wrap several shards in
        // Atomically). A failure undoes the statement on every shard. With
        // release_untouched, a shard the statement changed no row on is
        // left out of the transaction. Each shard's result goes in *results.
        ExecutionResult WriteShards(const std::map<uint32_t, std::string> &statements, bool release_untouched,
                                    std::map<uint32_t, ExecutionResult> *results);
        ExecutionResult Commit(Transaction *txn);
        void Abort(Transaction *txn);
        // Settle transactions an earlier leader left open on the shards
        void Recover();
        const std::map<std::string, TableInfo> &Tables(); // as txn_ sees them
        static TableInfo DescribeTable(const std::string &sql, const CreateTableStatement &create);
        net::RemoteConnection *NodeConnection(int node);
        std::unique_ptr<Database> SchemaDatabase(); // every table and index, empty
        ExecutionResult Ddl(const std::string &sql);
        ExecutionResult Insert(const std::string &sql);
        ExecutionResult Broadcast(const std::string &sql, const std::string &table);
        ExecutionResult Select(const std::string &sql);

        // How a SELECT runs on the cluster (D5)
        struct TableFetch
        {
            std::string table;
            std::vector<uint32_t> shards; // empty: no shard can hold a matching row
            std::string query;            // sent to each of them
        };
        struct SelectPlan
        {
            enum class Kind
            {
                SINGLE_SHARD,      // the whole statement on one shard
                PARTIAL_AGGREGATE, // aggregates per shard, combined here
                GATHER,            // fetch (filtered) rows, finish here
            } kind = Kind::GATHER;
            uint32_t shard = 0;                 // SINGLE_SHARD
            std::vector<uint32_t> shards;       // PARTIAL_AGGREGATE
            std::string shard_query;            // PARTIAL_AGGREGATE
            std::vector<AggregateFunction> functions; // PARTIAL_AGGREGATE, one per output column
            std::vector<TableFetch> fetches;    // GATHER
        };
        SelectPlan PlanSelect(const SelectStatement &select, const std::string &sql, Database &schema);
        std::string Describe(const SelectPlan &plan) const;
        ExecutionResult RunPartialAggregate(const SelectPlan &plan, const SelectStatement &select, Database &schema);
        // A copy of the expression with every (uncorrelated) subquery run on
        // the cluster and replaced by its result as literals; sets *error
        // if one fails
        std::unique_ptr<Expression> BindSubqueries(const Expression *expr, std::string *error);
        // UPDATE / DELETE with subqueries bound, as SQL
        std::string BoundUpdate(const UpdateStatement &update, std::string *error);
        std::string BoundDelete(const DeleteStatement &del, std::string *error);
        ExecutionResult Join(const std::vector<std::string> &arguments);
        std::vector<uint32_t> AllShards() const;

        Options options_;
        std::unique_ptr<ReplicatedShard> metadata_;
        std::unique_ptr<Server> server_;
        std::thread loop_;
        std::atomic<bool> stopping_{false};
        std::atomic<uint64_t> shard_reads_{0};
        std::atomic<uint64_t> two_phase_commits_{0};

        std::mutex op_mutex_; // one cluster operation at a time
        bool was_leader_ = false;
        View view_;
        std::map<int, std::unique_ptr<net::RemoteConnection>> connections_;
        std::map<uint32_t, int> shard_leaders_;
        std::map<std::pair<uint32_t, int>, bool> ensured_;
        std::chrono::steady_clock::time_point last_full_ensure_{};
        Transaction *txn_ = nullptr;           // the operation's transaction, if open
        std::set<std::string> active_txns_;    // open in clients' sessions (this term)
        std::set<Transaction *> explicit_txns_; // clients' BEGIN ... (this term), for the idle timeout
        void ExpireIdleTransactions();
        // Transactions waiting for a shard another one holds (waiter ->
        // holder); a cycle is a deadlock
        std::map<std::string, std::string> waits_for_;
        std::map<std::string, TableInfo> overlay_tables_;
        std::string overlay_key_;
        std::chrono::steady_clock::time_point last_recover_{};
        std::set<uint32_t> unsettled_; // shards that may hold a transaction no session owns
    };

} // namespace sql::cluster
