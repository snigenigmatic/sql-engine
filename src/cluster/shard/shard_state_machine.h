#pragma once

#include "catalog/database.h"
#include "cluster/raft/nuraft.h"
#include "execution/executor.h"
#include "session/session.h"
#include <atomic>
#include <memory>
#include <mutex>
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

        // Log entry payloads
        static nuraft::ptr<nuraft::buffer> EncodeWrite(const std::string &sql);
        static nuraft::ptr<nuraft::buffer> EncodeBarrier(); // applies nothing
        // The result an entry's commit returned (an error if malformed)
        static ExecutionResult DecodeResult(nuraft::buffer *result);

        // A read-only statement (SELECT / EXPLAIN) against this replica's
        // data as applied so far. Callers wanting the latest committed data
        // first pass a barrier through the log (ReplicatedShard::Read).
        ExecutionResult Query(const std::string &sql);

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
        };

        // Caller holds mutex_
        void OpenDatabase();
        void CloseDatabase();
        ExecutionResult Apply(nuraft::ulong index, const std::string &sql);
        // Sets __raft_meta to the index, in the open transaction if any
        void RecordApplied(nuraft::ulong index);

        static constexpr const char *META_TABLE = "__raft_meta";
        std::string SnapshotPath(nuraft::ulong index, const char *suffix) const;
        void DropOldSnapshots(nuraft::ulong keep);

        std::string dir_;
        size_t snapshot_chunk_;
        std::mutex mutex_; // one statement at a time: the engine has one writer
        std::unique_ptr<Database> db_;
        std::unique_ptr<Session> session_;
        std::atomic<nuraft::ulong> last_applied_{0};
        // The snapshot being received (one at a time)
        bool receiving_ok_ = false;
        uint64_t expected_size_ = 0;
        uint32_t expected_crc_ = 0;
        nuraft::ptr<nuraft::snapshot> last_snapshot_;
    };

} // namespace sql::cluster
