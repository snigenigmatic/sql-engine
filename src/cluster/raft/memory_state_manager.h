#pragma once

#include "cluster/raft/memory_log_store.h"
#include "cluster/raft/nuraft.h"
#include <mutex>
#include <string>

namespace sql::cluster
{

    // Keeps a Raft server's membership, vote and term in memory, with an
    // in-memory log. Nothing survives the process: for tests.
    class MemoryStateManager : public nuraft::state_mgr
    {
    public:
        // `endpoint` is how the other members reach this server (host:port)
        MemoryStateManager(int server_id, const std::string &endpoint);

        nuraft::ptr<nuraft::cluster_config> load_config() override;
        void save_config(const nuraft::cluster_config &config) override;
        void save_state(const nuraft::srv_state &state) override;
        nuraft::ptr<nuraft::srv_state> read_state() override;
        nuraft::ptr<nuraft::log_store> load_log_store() override { return log_store_; }
        nuraft::int32 server_id() override { return id_; }
        void system_exit(const int exit_code) override;

        nuraft::ptr<MemoryLogStore> GetLogStore() const { return log_store_; }
        nuraft::ptr<nuraft::srv_config> GetServerConfig() const { return server_config_; }

    private:
        std::mutex mutex_;
        int id_;
        nuraft::ptr<nuraft::srv_config> server_config_;
        nuraft::ptr<nuraft::cluster_config> config_;
        nuraft::ptr<nuraft::buffer> saved_state_; // serialized srv_state
        nuraft::ptr<MemoryLogStore> log_store_;
    };

} // namespace sql::cluster
