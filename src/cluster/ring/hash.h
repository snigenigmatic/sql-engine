#pragma once

#include "common/value.h"
#include <cstdint>
#include <string_view>

namespace sql::cluster
{

    struct Hash128
    {
        uint64_t low = 0;
        uint64_t high = 0;
    };

    // MurmurHash3, x64 128-bit variant (Austin Appleby's public-domain
    // algorithm): the same output as the reference implementation on
    // little-endian machines
    Hash128 Murmur3_128(const void *data, size_t size, uint32_t seed = 0);

    inline uint64_t Hash64(std::string_view bytes, uint32_t seed = 0)
    {
        return Murmur3_128(bytes.data(), bytes.size(), seed).low;
    }

    // Hash of a shard key. Values hash alike exactly when GROUP BY treats
    // them as one (1 and 1.0, 0.0 and -0.0, every NULL), so equal keys
    // always land on the same shard.
    uint64_t HashKey(const Value &value);

} // namespace sql::cluster
