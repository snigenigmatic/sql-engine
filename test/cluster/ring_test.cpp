#include <gtest/gtest.h>
#include "cluster/ring/hash.h"
#include "cluster/ring/hash_ring.h"
#include "cluster/ring/placement.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace sql::cluster
{

    namespace
    {
        // Population standard deviation over mean
        double Spread(const std::vector<double> &counts)
        {
            double mean = 0;
            for (double c : counts)
                mean += c;
            mean /= static_cast<double>(counts.size());
            double variance = 0;
            for (double c : counts)
                variance += (c - mean) * (c - mean);
            return std::sqrt(variance / static_cast<double>(counts.size())) / mean;
        }

        std::set<NodeId> AsSet(const std::vector<NodeId> &nodes) { return {nodes.begin(), nodes.end()}; }

        Placement Cluster(uint32_t shards, NodeId nodes, size_t rf = 3)
        {
            Placement placement(shards, rf);
            for (NodeId n = 0; n < nodes; ++n)
                placement.AddNode(n, "node-" + std::to_string(n));
            return placement;
        }
    } // namespace

    // Reference outputs of MurmurHash3_x64_128 (Python mmh3.hash64), covering
    // every tail length class: empty, 1, 5, 16 (one block), 17, 43
    TEST(HashTest, MatchesTheReferenceMurmur3)
    {
        struct Vector
        {
            std::string input;
            uint32_t seed;
            uint64_t low, high;
        };
        const std::vector<Vector> vectors = {
            {"", 0, 0x0, 0x0},
            {"a", 0, 0x85555565f6597889ULL, 0xe6b53a48510e895aULL},
            {"hello", 0, 0xcbd8a7b341bd9b02ULL, 0x5b1e906a48ae1d19ULL},
            {"The quick brown fox jumps over the lazy dog", 0, 0xe34bbc7bbc071b6cULL, 0x7a433ca9c49a9347ULL},
            {"0123456789abcdef", 0, 0x4be06d94cf4ad1a7ULL, 0x87c35b5c63a708daULL},
            {"0123456789abcdefX", 0, 0xcdebd2acb570d6f7ULL, 0x8f72119782104b27ULL},
            {"hello", 42, 0xc4b8b3c960af6f08ULL, 0x2334b875b0efbc7aULL},
        };
        for (const auto &v : vectors)
        {
            const Hash128 h = Murmur3_128(v.input.data(), v.input.size(), v.seed);
            EXPECT_EQ(h.low, v.low) << '"' << v.input << '"';
            EXPECT_EQ(h.high, v.high) << '"' << v.input << '"';
        }
        EXPECT_EQ(Hash64("hello"), 0xcbd8a7b341bd9b02ULL);
    }

    TEST(HashTest, EqualKeysHashAlike)
    {
        // As GROUP BY: numbers by value, every NULL alike
        EXPECT_EQ(HashKey(Value(1)), HashKey(Value(1.0)));
        EXPECT_EQ(HashKey(Value(0.0)), HashKey(Value(-0.0)));
        EXPECT_EQ(HashKey(Value(DataType::INTEGER)), HashKey(Value(DataType::VARCHAR)));
        // ...and different keys differently
        EXPECT_NE(HashKey(Value(1)), HashKey(Value(2)));
        EXPECT_NE(HashKey(Value(1)), HashKey(Value("1")));
        EXPECT_NE(HashKey(Value(true)), HashKey(Value(1)));
        EXPECT_NE(HashKey(Value("ab")), HashKey(Value("ba")));
    }

    TEST(HashRingTest, OwnsTheArcBeforeEachToken)
    {
        HashRing ring;
        EXPECT_THROW(ring.OwnerOf(0), std::logic_error);
        EXPECT_TRUE(ring.Preference(0, 3).empty());
        ring.Add(7, "only", 4);
        EXPECT_EQ(ring.TokenCount(), 4u);
        for (uint64_t h : {uint64_t{0}, uint64_t{1} << 63, ~uint64_t{0}})
            EXPECT_EQ(ring.OwnerOf(h), 7u);

        // Two owners with one token each: a hash goes to the next token
        // clockwise, and past the last token wraps to the first
        HashRing two;
        two.Add(1, "a", 1);
        two.Add(2, "b", 1);
        const uint64_t ta = Hash64("a#0"), tb = Hash64("b#0");
        const uint64_t lo = std::min(ta, tb), hi = std::max(ta, tb);
        const HashRing::OwnerId at_lo = ta < tb ? 1 : 2, at_hi = ta < tb ? 2 : 1;
        EXPECT_EQ(two.OwnerOf(lo), at_lo); // a token owns its own position
        EXPECT_EQ(two.OwnerOf(lo + 1), at_hi);
        EXPECT_EQ(two.OwnerOf(hi), at_hi);
        if (hi != ~uint64_t{0})
        {
            EXPECT_EQ(two.OwnerOf(hi + 1), at_lo); // wraps around
        }
        if (lo != 0)
        {
            EXPECT_EQ(two.OwnerOf(lo - 1), at_lo);
        }
    }

    TEST(HashRingTest, SameOwnersSameRingInAnyOrder)
    {
        HashRing forward, backward;
        for (uint32_t i = 0; i < 10; ++i)
            forward.Add(i, "n" + std::to_string(i), 32);
        for (uint32_t i = 10; i-- > 0;)
            backward.Add(i, "n" + std::to_string(i), 32);
        for (uint64_t h = 0; h < 2000; ++h)
        {
            const uint64_t hash = Hash64(std::to_string(h));
            EXPECT_EQ(forward.OwnerOf(hash), backward.OwnerOf(hash));
            EXPECT_EQ(forward.Preference(hash, 3), backward.Preference(hash, 3));
        }
    }

    TEST(HashRingTest, PreferenceListsAreDistinctOwners)
    {
        HashRing ring;
        for (uint32_t i = 0; i < 4; ++i)
            ring.Add(i, "n" + std::to_string(i), 16);
        for (uint64_t h = 0; h < 500; ++h)
        {
            const auto three = ring.Preference(Hash64(std::to_string(h)), 3);
            ASSERT_EQ(three.size(), 3u);
            EXPECT_EQ(AsSet(three).size(), 3u);
            EXPECT_EQ(three.front(), ring.OwnerOf(Hash64(std::to_string(h))));
            // More than there are owners: all of them, once each
            EXPECT_EQ(AsSet(ring.Preference(Hash64(std::to_string(h)), 9)).size(), 4u);
        }
        // Removing an owner removes all its tokens
        EXPECT_TRUE(ring.Remove(2));
        EXPECT_FALSE(ring.Remove(2));
        EXPECT_EQ(ring.TokenCount(), 48u);
        for (uint64_t h = 0; h < 500; ++h)
            EXPECT_NE(ring.OwnerOf(Hash64(std::to_string(h))), 2u);
    }

    TEST(PlacementTest, KeysSpreadEvenlyOverShards)
    {
        const Placement placement(16);
        std::vector<double> counts(16);
        for (int32_t key = 0; key < 200000; ++key)
            ++counts[placement.ShardFor(Value(key))];
        EXPECT_LT(Spread(counts), 0.10);
        for (double c : counts)
            EXPECT_GT(c, 0);
        // Text keys too
        std::vector<double> text(16);
        for (int key = 0; key < 200000; ++key)
            ++text[placement.ShardFor(Value("user-" + std::to_string(key)))];
        EXPECT_LT(Spread(text), 0.10);
    }

    TEST(PlacementTest, RowsNeverMoveBetweenShards)
    {
        Placement placement = Cluster(16, 3);
        std::vector<ShardId> before;
        for (int32_t key = 0; key < 1000; ++key)
            before.push_back(placement.ShardFor(Value(key)));
        placement.AddNode(3, "node-3");
        placement.RemoveNode(0);
        for (int32_t key = 0; key < 1000; ++key)
            EXPECT_EQ(placement.ShardFor(Value(key)), before[static_cast<size_t>(key)]);
        // Equal keys of different types share a shard
        EXPECT_EQ(placement.ShardFor(Value(5)), placement.ShardFor(Value(5.0)));
    }

    TEST(PlacementTest, ReplicasAreDistinctNodes)
    {
        Placement placement(16);
        EXPECT_TRUE(placement.ReplicasOf(0).empty()); // no nodes yet
        placement.AddNode(0, "node-0");
        placement.AddNode(1, "node-1");
        EXPECT_EQ(placement.ReplicasOf(3).size(), 2u); // fewer nodes than the factor
        placement.AddNode(2, "node-2");
        placement.AddNode(3, "node-3");
        placement.AddNode(4, "node-4");
        for (const auto &[shard, replicas] : placement.Assignment())
        {
            EXPECT_EQ(replicas.size(), 3u) << shard;
            EXPECT_EQ(AsSet(replicas).size(), 3u) << shard;
        }
        EXPECT_THROW(placement.ReplicasOf(16), std::out_of_range);
        // The replication factor decides the size of every replica set
        for (size_t rf : {size_t{1}, size_t{5}})
        {
            const Placement other = Cluster(16, 6, rf);
            EXPECT_EQ(other.ReplicationFactor(), rf);
            for (const auto &[shard, replicas] : other.Assignment())
                EXPECT_EQ(AsSet(replicas).size(), rf) << "rf " << rf << " shard " << shard;
        }
        EXPECT_THROW(Placement(0), std::invalid_argument);
    }

    TEST(PlacementTest, SameMembersSamePlacementEverywhere)
    {
        Placement a(32), b(32);
        for (NodeId n = 0; n < 5; ++n)
            a.AddNode(n, "node-" + std::to_string(n));
        for (NodeId n = 5; n-- > 0;)
            b.AddNode(n, "node-" + std::to_string(n));
        EXPECT_EQ(a.Assignment(), b.Assignment());
    }

    TEST(PlacementTest, ANewNodeOnlyJoinsReplicaSets)
    {
        Placement placement = Cluster(64, 4);
        const auto before = placement.Assignment();
        const uint64_t epoch = placement.Epoch();
        placement.AddNode(4, "node-4");
        EXPECT_GT(placement.Epoch(), epoch);
        const auto after = placement.Assignment();
        size_t moved = 0;
        for (const auto &[shard, replicas] : after)
        {
            const std::set<NodeId> old_set = AsSet(before.at(shard)), new_set = AsSet(replicas);
            std::vector<NodeId> gained, lost;
            std::set_difference(new_set.begin(), new_set.end(), old_set.begin(), old_set.end(),
                                std::back_inserter(gained));
            std::set_difference(old_set.begin(), old_set.end(), new_set.begin(), new_set.end(),
                                std::back_inserter(lost));
            // Only the new node comes in, pushing out at most one old one
            EXPECT_TRUE(gained.empty() || gained == std::vector<NodeId>{4}) << shard;
            EXPECT_EQ(gained.size(), lost.size()) << shard;
            moved += gained.size();
        }
        // The new node takes its share: about 64 shards × 3 replicas / 5 nodes
        const double expected = 64.0 * 3 / 5;
        EXPECT_GT(moved, expected * 0.5);
        EXPECT_LT(moved, expected * 1.5);
    }

    TEST(PlacementTest, ARemovedNodeIsReplacedAndNothingElseMoves)
    {
        Placement placement = Cluster(64, 5);
        const auto before = placement.Assignment();
        EXPECT_TRUE(placement.RemoveNode(2));
        EXPECT_FALSE(placement.RemoveNode(2));
        const auto after = placement.Assignment();
        for (const auto &[shard, replicas] : after)
        {
            const std::set<NodeId> old_set = AsSet(before.at(shard)), new_set = AsSet(replicas);
            EXPECT_EQ(new_set.count(2), 0u) << shard;
            // Every surviving member stays
            for (NodeId n : old_set)
            {
                if (n != 2)
                {
                    EXPECT_EQ(new_set.count(n), 1u) << shard << " lost node " << n;
                }
            }
            EXPECT_EQ(new_set.size(), 3u) << shard;
        }
    }

    TEST(PlacementTest, ReplicasSpreadOverNodes)
    {
        const Placement placement = Cluster(64, 5);
        std::vector<double> per_node(5);
        for (const auto &[shard, replicas] : placement.Assignment())
            for (NodeId n : replicas)
                ++per_node[n];
        const double mean = 64.0 * 3 / 5;
        for (double c : per_node)
        {
            EXPECT_GT(c, mean * 0.6);
            EXPECT_LT(c, mean * 1.4);
        }
    }

} // namespace sql::cluster
