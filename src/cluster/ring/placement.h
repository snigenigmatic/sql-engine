#pragma once

#include "cluster/ring/hash_ring.h"
#include "common/value.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sql::cluster
{

    using ShardId = uint32_t;
    using NodeId = uint32_t;

    // Where data lives, from two consistent-hash rings:
    //
    //   key ring:  a row's shard-key hash → shard. The shard count is fixed
    //              when the cluster is created, so a row never changes
    //              shards when nodes come and go.
    //   node ring: shard → the nodes holding its replicas, the first
    //              `replication_factor` distinct nodes clockwise from
    //              Hash64("shard-<id>"). A new node joins only the replica
    //              sets it lands in front of (about 1/N of them); a removed
    //              node's place goes to the next node along.
    //
    // Both rings are built from names alone, so every process that knows
    // the same nodes computes the same placement.
    class Placement
    {
    public:
        static constexpr size_t SHARD_VNODES = 256;
        static constexpr size_t NODE_VNODES = 128;

        explicit Placement(uint32_t shard_count, size_t replication_factor = 3);

        uint32_t ShardCount() const { return shard_count_; }
        size_t ReplicationFactor() const { return replication_factor_; }

        ShardId ShardForHash(uint64_t hash) const { return key_ring_.OwnerOf(hash); }
        ShardId ShardFor(const Value &key) const;

        // Changes bump the epoch; adding a known node again only renames it
        void AddNode(NodeId id, const std::string &name);
        bool RemoveNode(NodeId id);
        size_t NodeCount() const { return nodes_.size(); }
        const std::map<NodeId, std::string> &Nodes() const { return nodes_; }

        // In preference order: the first is the preferred leader. Fewer than
        // the replication factor while the cluster has fewer nodes.
        std::vector<NodeId> ReplicasOf(ShardId shard) const;

        // Every shard's replicas
        std::map<ShardId, std::vector<NodeId>> Assignment() const;

        // Bumped by every membership change
        uint64_t Epoch() const { return epoch_; }

    private:
        uint32_t shard_count_;
        size_t replication_factor_;
        HashRing key_ring_;
        HashRing node_ring_;
        std::map<NodeId, std::string> nodes_;
        uint64_t epoch_ = 0;
    };

} // namespace sql::cluster
