#include "cluster/ring/hash_ring.h"
#include "cluster/ring/hash.h"
#include <algorithm>
#include <stdexcept>

namespace sql::cluster
{

    void HashRing::Add(OwnerId id, const std::string &name, size_t vnodes)
    {
        Remove(id);
        owners_[id] = name;
        for (size_t i = 0; i < vnodes; ++i)
            tokens_.emplace_back(Hash64(name + "#" + std::to_string(i)), id);
        // Equal tokens (vanishingly rare) are ordered by owner id, so the
        // ring is still the same everywhere
        std::sort(tokens_.begin(), tokens_.end());
    }

    bool HashRing::Remove(OwnerId id)
    {
        if (owners_.erase(id) == 0)
            return false;
        tokens_.erase(std::remove_if(tokens_.begin(), tokens_.end(),
                                     [id](const std::pair<uint64_t, OwnerId> &token) { return token.second == id; }),
                      tokens_.end());
        return true;
    }

    size_t HashRing::Successor(uint64_t hash) const
    {
        const auto it = std::lower_bound(tokens_.begin(), tokens_.end(), std::make_pair(hash, OwnerId{0}));
        return it == tokens_.end() ? 0 : static_cast<size_t>(it - tokens_.begin());
    }

    HashRing::OwnerId HashRing::OwnerOf(uint64_t hash) const
    {
        if (tokens_.empty())
            throw std::logic_error("hash ring has no owners");
        return tokens_[Successor(hash)].second;
    }

    std::vector<HashRing::OwnerId> HashRing::Preference(uint64_t hash, size_t count) const
    {
        std::vector<OwnerId> owners;
        count = std::min(count, owners_.size());
        if (tokens_.empty())
            return owners;
        for (size_t i = Successor(hash), seen = 0; owners.size() < count && seen < tokens_.size();
             i = (i + 1) % tokens_.size(), ++seen)
        {
            const OwnerId owner = tokens_[i].second;
            if (std::find(owners.begin(), owners.end(), owner) == owners.end())
                owners.push_back(owner);
        }
        return owners;
    }

} // namespace sql::cluster
