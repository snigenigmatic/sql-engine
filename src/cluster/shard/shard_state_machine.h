#pragma once

#include "catalog/database.h"
#include "cluster/raft/nuraft.h"
#include "execution/executor.h"
#include "session/session.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace sql::cluster
{

    // One replica of a shard: a database file that Raft log entries are
    // applied to, in log order, identically on every replica.
    //
    //   <dir>/shard.db            the data (and shard.db-wal)
    //   <dir>/snapshots/<i>.db    a copy of shard.db as of log index i,
    //   <dir>/snapshots/<i>.meta  and NuRaft's description of it
    //
    // Each write entry carries one SQL statement, its literals already
    // bound, and runs in a local transaction together with an update of
    // the __raft_meta table to the entry's index: the statement and "applied
    // up to here" become durable together, so after a crash nothing is
    // applied twice or lost (NuRaft replays the log from last_commit_index).
    // A statement that fails, fails the same way everywhere, and its index
    // is still recorded.
    class ShardStateMachine : public nuraft::state_machine
    {
    public:
        // Snapshots are sent in pieces of `snapshot_chunk` bytes. Throws
        // std::runtime_error if the database cannot be opened.
        explicit ShardStateMachine(const std::string &directory, size_t snapshot_chunk = 1 << 20);
        ~ShardStateMachine() override;

        // Log entry payloads. `request_id` (1-255 characters) makes a write
        // idempotent: a retry with the same id after an uncertain failure
        // (e.g. a leader change cancelled the request, but it committed)
        // returns the first outcome instead of applying again
        static nuraft::ptr<nuraft::buffer> EncodeWrite(const std::string &sql, const std::string &request_id);
        static nuraft::ptr<nuraft::buffer> EncodeBarrier(); // applies nothing

        // Several statements applied all or nothing: if one fails, none of
        // them stays and the result is that failure; otherwise the last
        // statement's result. For the coordinators' metadata.
        static nuraft::ptr<nuraft::buffer> EncodeBatch(const std::string &script, const std::string &request_id);

        // A step of a distributed transaction (two-phase commit). While a
        // transaction is open on a replica its changes stay uncommitted in
        // the database's own transaction, and the shard is locked: every
        // other write, and every read outside that transaction, is refused
        // with a "locked by another transaction" failure (callers retry).
        //
        //   EXEC     run sql in the transaction (opening it if needed); a
        //            failing statement is undone on its own
        //   UNDO     undo the last EXEC (the coordinator's statement failed
        //            on another shard)
        //   PREPARE  vote: succeeds if the transaction is open here; after it
        //            no more EXEC or UNDO
        //   COMMIT   commit it and unlock
        //   ABORT    roll it back and unlock (a no-op if it is not open)
        //
        // A replica that restarts replays its log, which reopens the
        // transaction exactly as it was.
        enum class TxnOp : uint8_t
        {
            EXEC = 0,
            UNDO = 1,
            PREPARE = 2,
            COMMIT = 3,
            ABORT = 4,
        };
        static nuraft::ptr<nuraft::buffer> EncodeTxn(TxnOp op, const std::string &txid, const std::string &sql,
                                                     const std::string &request_id);
        static bool ParseTxnOp(const std::string &name, TxnOp *op);

        // The id of the transaction open on this replica, or ""
        std::string OpenTransaction();
        // The result an entry's commit returned (an error if malformed)
        static ExecutionResult DecodeResult(nuraft::buffer *result);

        // A read-only statement (SELECT / EXPLAIN) against this replica's
        // data as applied so far. Callers wanting the latest committed data
        // first pass a barrier through the log (ReplicatedShard::Read).
        // Inside transaction `txid` (if not empty) it sees that
        // transaction's own changes.
        ExecutionResult Query(const std::string &sql, const std::string &txid = "");

        nuraft::ptr<nuraft::buffer> commit(const nuraft::ulong log_idx, nuraft::buffer &data) override;
        nuraft::ulong last_commit_index() override { return last_applied_; }

        void create_snapshot(nuraft::snapshot &s, nuraft::async_result<bool>::handler_type &when_done) override;
        nuraft::ptr<nuraft::snapshot> last_snapshot() override;
        int read_logical_snp_obj(nuraft::snapshot &s, void *&user_snp_ctx, nuraft::ulong obj_id,
                                 nuraft::ptr<nuraft::buffer> &data_out, bool &is_last_obj) override;
        void save_logical_snp_obj(nuraft::snapshot &s, nuraft::ulong &obj_id, nuraft::buffer &data,
                                  bool is_first_obj, bool is_last_obj) override;
        bool apply_snapshot(nuraft::snapshot &s) override;

    private:
        enum class Kind : uint8_t
        {
            BARRIER = 0,
            WRITE = 1,
            BATCH = 2,
            TXN = 3,
        };

        // Caller holds mutex_
        void OpenDatabase();
        void CloseDatabase();
        ExecutionResult Apply(nuraft::ulong index, const std::string &sql, const std::string &request_id, bool batch);
        ExecutionResult ApplyTxn(nuraft::ulong index, TxnOp op, const std::string &txid, const std::string &sql,
                                 const std::string &request_id);
        // Moves the applied index on, committing it unless a transaction is
        // open (then it commits, or rolls back, with the transaction)
        void Advance(nuraft::ulong index);
        ExecutionResult Locked() const;
        // The outcome recorded for a request id, if it was applied
        std::optional<ExecutionResult> Recorded(const std::string &request_id);
        void Record(nuraft::ulong index, const std::string &request_id, const ExecutionResult &result);
        // Sets __raft_meta to the index, in the open transaction if any
        void RecordApplied(nuraft::ulong index);

        static constexpr const char *META_TABLE = "__raft_meta";
        // Outcomes of recent writes by request id; entries older than
        // REQUEST_WINDOW log entries are pruned
        static constexpr const char *REQUESTS_TABLE = "__raft_requests";
        static constexpr nuraft::ulong REQUEST_WINDOW = 10000;
        std::string SnapshotPath(nuraft::ulong index, const char *suffix) const;
        void DropOldSnapshots(nuraft::ulong keep);

        std::string dir_;
        size_t snapshot_chunk_;
        std::mutex mutex_; // one statement at a time: the engine has one writer
        std::unique_ptr<Database> db_;
        std::unique_ptr<Session> session_;
        std::atomic<nuraft::ulong> last_applied_{0};
        // The open distributed transaction (see EncodeTxn)
        std::string open_txid_;
        bool prepared_ = false;
        bool has_statement_savepoint_ = false;
        Pager::Savepoint statement_savepoint_{};
        // The snapshot being received (one at a time)
        bool receiving_ok_ = false;
        uint64_t expected_size_ = 0;
        uint32_t expected_crc_ = 0;
        nuraft::ptr<nuraft::snapshot> last_snapshot_;
    };

} // namespace sql::cluster
