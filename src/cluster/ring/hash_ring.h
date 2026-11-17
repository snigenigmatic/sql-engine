#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sql::cluster
{

    // A consistent-hash ring. Each owner puts `vnodes` tokens on a 64-bit
    // circle, at Hash64("<name>#<i>"); a hash belongs to the owner of the
    // first token at or after it, wrapping around. Tokens depend only on
    // owner names, so every process with the same owners builds the same
    // ring, whatever order they were added in. Adding an owner takes over
    // only the arcs in front of its own tokens.
    class HashRing
    {
    public:
        using OwnerId = uint32_t;

        // Re-adding an owner replaces its tokens
        void Add(OwnerId id, const std::string &name, size_t vnodes);
        // False if the owner was not on the ring
        bool Remove(OwnerId id);

        bool Contains(OwnerId id) const { return owners_.count(id) != 0; }
        size_t OwnerCount() const { return owners_.size(); }
        size_t TokenCount() const { return tokens_.size(); }

        // Owner of the first token at or after the hash. Throws
        // std::logic_error on an empty ring.
        OwnerId OwnerOf(uint64_t hash) const;

        // The first `count` distinct owners met walking clockwise from the
        // hash (Dynamo's preference list); all owners if there are fewer
        std::vector<OwnerId> Preference(uint64_t hash, size_t count) const;

    private:
        // Index of the first token at or after the hash (wrapping)
        size_t Successor(uint64_t hash) const;

        std::vector<std::pair<uint64_t, OwnerId>> tokens_; // sorted
        std::map<OwnerId, std::string> owners_;
    };

} // namespace sql::cluster
