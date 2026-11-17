#include "cluster/node/storage_node.h"
#include "net/remote_connection.h"
#include <chrono>
#include <filesystem>
#include <sstream>

namespace sql::cluster
{

    namespace
    {
        ExecutionResult Failure(const std::string &message) { return ExecutionResult{false, message, {}, {}}; }
        ExecutionResult Success(const std::string &message) { return ExecutionResult{true, message, {}, {}}; }

        // Each client of the node's server gets one of these
        class NodeConnection : public Connection
        {
        public:
            explicit NodeConnection(StorageNode *node) : node_(node) {}
            ExecutionResult Execute(const std::string &) override
            {
                return Failure("a storage node takes SQL from its coordinator only");
            }
            std::vector<ExecutionResult> ExecuteScript(const std::string &sql, bool) override { return {Execute(sql)}; }
            bool InTransaction() override { return false; }
            std::string Command(const std::string &name, const std::string &) override
            {
                return "Error: a storage node has no command " + name + "\n";
            }
            ExecutionResult Call(const std::string &method, const std::vector<std::string> &arguments) override
            {
                return node_->Handle(method, arguments);
            }

        private:
            StorageNode *node_;
        };

        std::pair<std::string, uint16_t> SplitAddress(const std::string &address)
        {
            const size_t colon = address.rfind(':');
            return {address.substr(0, colon), static_cast<uint16_t>(std::stoi(address.substr(colon + 1)))};
        }
    } // namespace

    std::string EncodeMembers(const std::vector<ReplicatedShard::Member> &members)
    {
        std::string text;
        for (const auto &member : members)
            text += (text.empty() ? "" : ",") + std::to_string(member.id) + "@" + member.endpoint;
        return text;
    }

    std::vector<ReplicatedShard::Member> DecodeMembers(const std::string &text)
    {
        std::vector<ReplicatedShard::Member> members;
        std::stringstream in(text);
        std::string item;
        while (std::getline(in, item, ','))
        {
            const size_t at = item.find('@');
            if (at == std::string::npos)
                throw std::invalid_argument("bad member " + item);
            members.push_back({std::stoi(item.substr(0, at)), item.substr(at + 1)});
        }
        return members;
    }

    int LeaderHint(const std::string &message)
    {
        const std::string marker = "(leader: ";
        const size_t at = message.find(marker);
        if (at == std::string::npos)
            return -1;
        try
        {
            return std::stoi(message.substr(at + marker.size()));
        }
        catch (const std::exception &)
        {
            return -1;
        }
    }

    StorageNode::StorageNode(const Options &options) : options_(options) {}

    StorageNode::~StorageNode() { Stop(); }

    void StorageNode::Start()
    {
        std::filesystem::create_directories(options_.directory);
        server_ = std::make_unique<Server>([this] { return std::make_unique<NodeConnection>(this); }, false);
        server_->Start(options_.host, options_.port);
        announcer_ = std::thread([this] { Announce(); });
    }

    void StorageNode::Stop()
    {
        if (stopping_.exchange(true))
            return;
        if (announcer_.joinable())
            announcer_.join();
        if (server_)
            server_->Stop();
        std::lock_guard<std::mutex> lock(mutex_);
        replicas_.clear();
    }

    ReplicatedShard *StorageNode::Replica(uint32_t shard)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = replicas_.find(shard);
        return it == replicas_.end() ? nullptr : it->second.get();
    }

    void StorageNode::Announce()
    {
        // Re-announcing is harmless (a known name keeps its id) and lets a
        // new coordinator leader learn the address after a failover
        while (!stopping_)
        {
            for (const auto &address : options_.coordinators)
            {
                if (stopping_)
                    break;
                try
                {
                    auto [host, port] = SplitAddress(address);
                    net::RemoteConnection coordinator(host, port);
                    const ExecutionResult joined = coordinator.Call(
                        "join", {options_.name, options_.host, std::to_string(Port()),
                                 std::to_string(options_.raft_port_base)});
                    if (joined.success)
                    {
                        node_id_ = std::stoi(joined.message);
                        break;
                    }
                }
                catch (const std::exception &)
                {
                    // that coordinator is down; try the next
                }
            }
            for (int i = 0; i < 10 && !stopping_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool StorageNode::CheckEpoch(const std::string &epoch, ExecutionResult *refusal)
    {
        const uint64_t seen = std::stoull(epoch);
        uint64_t current = epoch_.load();
        while (seen > current && !epoch_.compare_exchange_weak(current, seen))
        {
        }
        if (seen < epoch_.load())
        {
            *refusal = Failure("stale epoch " + epoch + " (node is at " + std::to_string(epoch_.load()) + ")");
            return false;
        }
        return true;
    }

    ExecutionResult StorageNode::Handle(const std::string &method, const std::vector<std::string> &arguments)
    {
        try
        {
            if (arguments.size() < 3)
                return Failure("bad call " + method);
            ExecutionResult refusal;
            if (!CheckEpoch(arguments[0], &refusal))
                return refusal;
            const uint32_t shard = static_cast<uint32_t>(std::stoul(arguments[1]));
            if (method == "ensure_replica")
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (replicas_.count(shard))
                    return Success("exists");
                ReplicatedShard::Options options;
                options.id = node_id_;
                options.port = options_.raft_port_base + static_cast<int>(shard);
                options.directory = options_.directory + "/shard-" + std::to_string(shard);
                options.members = DecodeMembers(arguments[2]);
                options.heartbeat_ms = options_.heartbeat_ms;
                options.election_timeout_ms = options_.election_timeout_ms;
                options.snapshot_distance = options_.snapshot_distance;
                if (options.id < 0)
                    return Failure("this node has not joined yet");
                replicas_[shard] = std::make_unique<ReplicatedShard>(options);
                return Success("created");
            }
            ReplicatedShard *replica = Replica(shard);
            if (!replica)
                return Failure("no replica of shard " + std::to_string(shard) + " here");
            if (method == "write") // the coordinator's request id makes retries safe
                return replica->Write(arguments[2], arguments.size() > 3 ? arguments[3] : "");
            if (method == "read")
                return replica->Read(arguments[2]);
            return Failure("unknown call " + method);
        }
        catch (const std::exception &e)
        {
            return Failure(e.what());
        }
    }

} // namespace sql::cluster
