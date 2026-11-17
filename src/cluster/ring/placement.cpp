#include "cluster/ring/placement.h"
#include "cluster/ring/hash.h"
#include <stdexcept>

namespace sql::cluster
{

    namespace
    {
        std::string ShardName(ShardId shard) { return "shard-" + std::to_string(shard); }
    } // namespace

    Placement::Placement(uint32_t shard_count, size_t replication_factor)
        : shard_count_(shard_count), replication_factor_(replication_factor)
    {
        if (shard_count == 0)
            throw std::invalid_argument("a cluster needs at least one shard");
        if (replication_factor == 0)
            throw std::invalid_argument("the replication factor must be at least 1");
        for (ShardId shard = 0; shard < shard_count; ++shard)
            key_ring_.Add(shard, ShardName(shard), SHARD_VNODES);
    }

    ShardId Placement::ShardFor(const Value &key) const { return ShardForHash(HashKey(key)); }

    void Placement::AddNode(NodeId id, const std::string &name)
    {
        auto it = nodes_.find(id);
        if (it != nodes_.end() && it->second == name)
            return;
        nodes_[id] = name;
        node_ring_.Add(id, name, NODE_VNODES);
        ++epoch_;
    }

    bool Placement::RemoveNode(NodeId id)
    {
        if (nodes_.erase(id) == 0)
            return false;
        node_ring_.Remove(id);
        ++epoch_;
        return true;
    }

    std::vector<NodeId> Placement::ReplicasOf(ShardId shard) const
    {
        if (shard >= shard_count_)
            throw std::out_of_range("no shard " + std::to_string(shard));
        return node_ring_.Preference(Hash64(ShardName(shard)), replication_factor_);
    }

    std::map<ShardId, std::vector<NodeId>> Placement::Assignment() const
    {
        std::map<ShardId, std::vector<NodeId>> assignment;
        for (ShardId shard = 0; shard < shard_count_; ++shard)
            assignment[shard] = ReplicasOf(shard);
        return assignment;
    }

} // namespace sql::cluster
