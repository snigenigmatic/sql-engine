#pragma once

#include "cluster/raft/nuraft.h"
#include "cluster/shard/file_log_store.h"
#include <mutex>
#include <string>

namespace sql::cluster
{

    // A Raft server's durable state in a directory: its vote and term
    // (<dir>/state), its view of the membership (<dir>/config), each
    // replaced atomically, and its log (<dir>/log).
    class FileStateManager : public nuraft::state_mgr
    {
    public:
        // `initial` is the membership to start from when the directory has
        // none yet (a fresh server); afterwards the saved one wins
        FileStateManager(int server_id, const std::string &directory, nuraft::ptr<nuraft::cluster_config> initial);

        nuraft::ptr<nuraft::cluster_config> load_config() override;
        void save_config(const nuraft::cluster_config &config) override;
        void save_state(const nuraft::srv_state &state) override;
        nuraft::ptr<nuraft::srv_state> read_state() override;
        nuraft::ptr<nuraft::log_store> load_log_store() override { return log_store_; }
        nuraft::int32 server_id() override { return id_; }
        void system_exit(const int exit_code) override;

        nuraft::ptr<FileLogStore> GetLogStore() const { return log_store_; }

    private:
        std::mutex mutex_;
        int id_;
        std::string dir_;
        nuraft::ptr<nuraft::cluster_config> config_;
        nuraft::ptr<FileLogStore> log_store_;
    };

} // namespace sql::cluster
