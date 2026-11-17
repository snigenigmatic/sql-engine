#include "cluster/raft/memory_state_manager.h"
#include <stdexcept>

namespace sql::cluster
{

    MemoryStateManager::MemoryStateManager(int server_id, const std::string &endpoint)
        : id_(server_id), server_config_(nuraft::cs_new<nuraft::srv_config>(server_id, endpoint)),
          config_(nuraft::cs_new<nuraft::cluster_config>()), log_store_(nuraft::cs_new<MemoryLogStore>())
    {
        // A new server starts as the only member of its own cluster; others
        // join it through add_srv
        config_->get_servers().push_back(server_config_);
    }

    nuraft::ptr<nuraft::cluster_config> MemoryStateManager::load_config()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

    void MemoryStateManager::save_config(const nuraft::cluster_config &config)
    {
        nuraft::ptr<nuraft::buffer> copy = config.serialize();
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = nuraft::cluster_config::deserialize(*copy);
    }

    void MemoryStateManager::save_state(const nuraft::srv_state &state)
    {
        nuraft::ptr<nuraft::buffer> copy = state.serialize();
        std::lock_guard<std::mutex> lock(mutex_);
        saved_state_ = copy;
    }

    nuraft::ptr<nuraft::srv_state> MemoryStateManager::read_state()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return saved_state_ ? nuraft::srv_state::deserialize(*saved_state_) : nullptr;
    }

    void MemoryStateManager::system_exit(const int exit_code)
    {
        // NuRaft asks for this only on unrecoverable internal errors
        throw std::runtime_error("Raft server " + std::to_string(id_) + " requested exit (code " +
                                 std::to_string(exit_code) + ")");
    }

} // namespace sql::cluster
