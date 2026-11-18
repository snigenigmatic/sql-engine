#include "cluster/shard/replicated_shard.h"
#include "cluster/raft/raft_logger.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <filesystem>
#include <random>
#include <stdexcept>

namespace sql::cluster
{

    namespace
    {
        ExecutionResult Failure(const std::string &message)
        {
            ExecutionResult result;
            result.success = false;
            result.message = message;
            return result;
        }
    } // namespace

    ReplicatedShard::ReplicatedShard(const Options &options) : options_(options)
    {
        std::filesystem::create_directories(options.directory);
        machine_ = nuraft::cs_new<ShardStateMachine>(options.directory + "/data", options.snapshot_chunk);

        auto members = nuraft::cs_new<nuraft::cluster_config>();
        if (options.joining) // the leader sends the real membership when it adds us
            members->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(options.id, options.endpoint));
        else
            for (const auto &member : options.members)
                members->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(member.id, member.endpoint));
        state_ = nuraft::cs_new<FileStateManager>(options.id, options.directory, members);

        nuraft::raft_params params;
        params.heart_beat_interval_ = options.heartbeat_ms;
        params.election_timeout_lower_bound_ = options.election_timeout_ms;
        params.election_timeout_upper_bound_ = options.election_timeout_ms * 2;
        params.snapshot_distance_ = options.snapshot_distance;
        params.reserved_log_items_ = options.snapshot_distance > 0 ? options.snapshot_distance / 2 : 0;
        params.client_req_timeout_ = options.election_timeout_ms * 3;
        params.return_method_ = nuraft::raft_params::blocking;
        nuraft::asio_service::options asio;
        asio.thread_pool_size_ = 2;

        launcher_ = std::make_unique<nuraft::raft_launcher>();
        nuraft::raft_server::init_options init;
        init.skip_initial_election_timeout_ = options.joining;
        raft_ = launcher_->init(machine_, state_, nuraft::cs_new<RaftLogger>(options.log_level), options.port, asio,
                                params, init);
        if (!raft_)
            throw std::runtime_error("cannot start Raft server " + std::to_string(options.id) + " on port " +
                                     std::to_string(options.port));
    }

    ReplicatedShard::~ReplicatedShard() { Stop(); }

    void ReplicatedShard::Stop()
    {
        if (!raft_)
            return;
        launcher_->shutdown(5);
        raft_.reset();
    }

    bool ReplicatedShard::IsLeader() const { return raft_ && raft_->is_leader(); }

    namespace
    {
        bool HasMember(const nuraft::ptr<nuraft::raft_server> &raft, int id)
        {
            std::vector<nuraft::ptr<nuraft::srv_config>> members;
            raft->get_srv_config_all(members);
            for (const auto &member : members)
                if (member->get_id() == id)
                    return true;
            return false;
        }
    } // namespace

    ExecutionResult ReplicatedShard::Members()
    {
        if (!IsLeader())
            return Failure("not the leader (leader: " + std::to_string(LeaderId()) + ")");
        std::vector<nuraft::ptr<nuraft::srv_config>> members;
        raft_->get_srv_config_all(members);
        std::vector<int> ids;
        for (const auto &member : members)
            ids.push_back(member->get_id());
        std::sort(ids.begin(), ids.end());
        std::string text;
        for (int id : ids)
            text += (text.empty() ? "" : ",") + std::to_string(id);
        ExecutionResult result;
        result.success = true;
        result.message = text;
        return result;
    }

    ExecutionResult ReplicatedShard::AddMember(int id, const std::string &endpoint)
    {
        if (!IsLeader())
            return Failure("not the leader (leader: " + std::to_string(LeaderId()) + ")");
        ExecutionResult done;
        done.success = true;
        done.message = "member";
        if (HasMember(raft_, id))
            return done;
        auto result = raft_->add_srv(nuraft::srv_config(id, endpoint));
        if (!result->get_accepted() && result->get_result_code() != nuraft::cmd_result_code::SERVER_ALREADY_EXISTS)
            return Failure("pending: adding replica " + std::to_string(id) + " (" + result->get_result_str() + ")");
        // NuRaft catches the new member up, then commits the new membership
        for (int i = 0; i < 40 && !HasMember(raft_, id); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return HasMember(raft_, id) ? done : Failure("pending: replica " + std::to_string(id) + " is catching up");
    }

    ExecutionResult ReplicatedShard::RemoveMember(int id)
    {
        if (!IsLeader())
            return Failure("not the leader (leader: " + std::to_string(LeaderId()) + ")");
        ExecutionResult done;
        done.success = true;
        done.message = "removed";
        if (!HasMember(raft_, id))
            return done;
        if (id == options_.id)
        {
            // A leader cannot remove itself: hand over first
            raft_->yield_leadership();
            return Failure("pending: replica " + std::to_string(id) + " is handing over its leadership");
        }
        auto result = raft_->remove_srv(id);
        if (!result->get_accepted() && result->get_result_code() != nuraft::cmd_result_code::SERVER_NOT_FOUND)
            return Failure("pending: removing replica " + std::to_string(id) + " (" + result->get_result_str() + ")");
        for (int i = 0; i < 40 && HasMember(raft_, id); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return HasMember(raft_, id) ? Failure("pending: removing replica " + std::to_string(id)) : done;
    }

    int ReplicatedShard::LeaderId() const { return raft_ ? raft_->get_leader() : -1; }

    ExecutionResult ReplicatedShard::Replicate(nuraft::ptr<nuraft::buffer> entry)
    {
        if (!raft_)
            return Failure("replica " + std::to_string(options_.id) + " is stopped");
        auto result = raft_->append_entries({entry});
        if (!result->get_accepted() || result->get_result_code() == nuraft::cmd_result_code::NOT_LEADER)
            return Failure("not the leader (leader: " + std::to_string(LeaderId()) + ")");
        if (result->get_result_code() != nuraft::cmd_result_code::OK)
            return Failure("not committed: " + result->get_result_str());
        return ShardStateMachine::DecodeResult(result->get().get());
    }

    std::string ReplicatedShard::NewRequestId()
    {
        static thread_local std::mt19937_64 random(std::random_device{}());
        char id[33];
        std::snprintf(id, sizeof(id), "%016llx%016llx", static_cast<unsigned long long>(random()),
                      static_cast<unsigned long long>(random()));
        return id;
    }

    ExecutionResult ReplicatedShard::Write(const std::string &sql, const std::string &request_id)
    {
        return Replicate(ShardStateMachine::EncodeWrite(sql, request_id.empty() ? NewRequestId() : request_id));
    }

    ExecutionResult ReplicatedShard::WriteBatch(const std::string &script, const std::string &request_id)
    {
        return Replicate(ShardStateMachine::EncodeBatch(script, request_id.empty() ? NewRequestId() : request_id));
    }

    ExecutionResult ReplicatedShard::Transaction(ShardStateMachine::TxnOp op, const std::string &txid,
                                                 const std::string &sql, const std::string &request_id)
    {
        return Replicate(
            ShardStateMachine::EncodeTxn(op, txid, sql, request_id.empty() ? NewRequestId() : request_id));
    }

    ExecutionResult ReplicatedShard::OpenTransaction()
    {
        ExecutionResult barrier = Replicate(ShardStateMachine::EncodeBarrier());
        if (!barrier.success)
            return barrier;
        barrier.message = machine_->OpenTransaction();
        return barrier;
    }

    ExecutionResult ReplicatedShard::Read(const std::string &sql, const std::string &txid)
    {
        ExecutionResult barrier = Replicate(ShardStateMachine::EncodeBarrier());
        if (!barrier.success)
            return barrier;
        return machine_->Query(sql, txid);
    }

} // namespace sql::cluster
