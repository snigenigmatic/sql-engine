#include "cluster/shard/replicated_shard.h"
#include "cluster/raft/raft_logger.h"
#include <filesystem>
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
        raft_ = launcher_->init(machine_, state_, nuraft::cs_new<RaftLogger>(options.log_level), options.port, asio,
                                params);
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

    ExecutionResult ReplicatedShard::Write(const std::string &sql)
    {
        return Replicate(ShardStateMachine::EncodeWrite(sql));
    }

    ExecutionResult ReplicatedShard::Read(const std::string &sql)
    {
        ExecutionResult barrier = Replicate(ShardStateMachine::EncodeBarrier());
        if (!barrier.success)
            return barrier;
        return machine_->Query(sql);
    }

} // namespace sql::cluster
