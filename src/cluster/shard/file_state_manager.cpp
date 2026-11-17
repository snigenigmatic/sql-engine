#include "cluster/shard/file_state_manager.h"
#include <stdexcept>

namespace sql::cluster
{

    namespace
    {
        std::string Bytes(const nuraft::ptr<nuraft::buffer> &buffer)
        {
            return std::string(reinterpret_cast<const char *>(buffer->data_begin()), buffer->size());
        }

        nuraft::ptr<nuraft::buffer> Buffer(const std::string &bytes)
        {
            nuraft::ptr<nuraft::buffer> buffer = nuraft::buffer::alloc(bytes.size());
            buffer->put_raw(reinterpret_cast<const nuraft::byte *>(bytes.data()), bytes.size());
            buffer->pos(0);
            return buffer;
        }
    } // namespace

    FileStateManager::FileStateManager(int server_id, const std::string &directory,
                                       nuraft::ptr<nuraft::cluster_config> initial)
        : id_(server_id), dir_(directory), log_store_(nuraft::cs_new<FileLogStore>(directory))
    {
        std::string saved;
        if (ReadFile(dir_ + "/config", &saved) && !saved.empty())
        {
            nuraft::ptr<nuraft::buffer> buffer = Buffer(saved);
            config_ = nuraft::cluster_config::deserialize(*buffer);
        }
        else
            config_ = initial;
    }

    nuraft::ptr<nuraft::cluster_config> FileStateManager::load_config()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }

    void FileStateManager::save_config(const nuraft::cluster_config &config)
    {
        nuraft::ptr<nuraft::buffer> bytes = config.serialize();
        std::lock_guard<std::mutex> lock(mutex_);
        WriteFileAtomically(dir_ + "/config", Bytes(bytes));
        config_ = nuraft::cluster_config::deserialize(*bytes);
    }

    void FileStateManager::save_state(const nuraft::srv_state &state)
    {
        nuraft::ptr<nuraft::buffer> bytes = state.serialize();
        std::lock_guard<std::mutex> lock(mutex_);
        WriteFileAtomically(dir_ + "/state", Bytes(bytes));
    }

    nuraft::ptr<nuraft::srv_state> FileStateManager::read_state()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string saved;
        if (!ReadFile(dir_ + "/state", &saved) || saved.empty())
            return nullptr;
        nuraft::ptr<nuraft::buffer> buffer = Buffer(saved);
        return nuraft::srv_state::deserialize(*buffer);
    }

    void FileStateManager::system_exit(const int exit_code)
    {
        throw std::runtime_error("Raft server " + std::to_string(id_) + " requested exit (code " +
                                 std::to_string(exit_code) + ")");
    }

} // namespace sql::cluster
