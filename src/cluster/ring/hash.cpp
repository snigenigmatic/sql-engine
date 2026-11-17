#include "cluster/ring/hash.h"
#include "execution/evaluator.h"
#include <cstring>

namespace sql::cluster
{

    namespace
    {
        inline uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

        inline uint64_t Mix(uint64_t k)
        {
            k ^= k >> 33;
            k *= 0xff51afd7ed558ccdULL;
            k ^= k >> 33;
            k *= 0xc4ceb9fe1a85ec53ULL;
            k ^= k >> 33;
            return k;
        }

        inline uint64_t Load64(const unsigned char *p)
        {
            uint64_t v;
            std::memcpy(&v, p, sizeof(v)); // little-endian hosts, as the reference
            return v;
        }
    } // namespace

    Hash128 Murmur3_128(const void *data, size_t size, uint32_t seed)
    {
        const auto *bytes = static_cast<const unsigned char *>(data);
        const size_t blocks = size / 16;
        uint64_t h1 = seed, h2 = seed;
        const uint64_t c1 = 0x87c37b91114253d5ULL, c2 = 0x4cf5ad432745937fULL;

        for (size_t i = 0; i < blocks; ++i)
        {
            uint64_t k1 = Load64(bytes + i * 16), k2 = Load64(bytes + i * 16 + 8);
            k1 *= c1;
            k1 = Rotl(k1, 31);
            k1 *= c2;
            h1 ^= k1;
            h1 = Rotl(h1, 27);
            h1 += h2;
            h1 = h1 * 5 + 0x52dce729;
            k2 *= c2;
            k2 = Rotl(k2, 33);
            k2 *= c1;
            h2 ^= k2;
            h2 = Rotl(h2, 31);
            h2 += h1;
            h2 = h2 * 5 + 0x38495ab5;
        }

        // The last 0..15 bytes
        const unsigned char *tail = bytes + blocks * 16;
        const size_t rest = size & 15;
        uint64_t k1 = 0, k2 = 0;
        for (size_t i = rest; i > 8; --i)
            k2 |= uint64_t{tail[i - 1]} << ((i - 9) * 8);
        if (rest > 8)
        {
            k2 *= c2;
            k2 = Rotl(k2, 33);
            k2 *= c1;
            h2 ^= k2;
        }
        for (size_t i = rest < 8 ? rest : 8; i > 0; --i)
            k1 |= uint64_t{tail[i - 1]} << ((i - 1) * 8);
        if (rest > 0)
        {
            k1 *= c1;
            k1 = Rotl(k1, 31);
            k1 *= c2;
            h1 ^= k1;
        }

        h1 ^= size;
        h2 ^= size;
        h1 += h2;
        h2 += h1;
        h1 = Mix(h1);
        h2 = Mix(h2);
        h1 += h2;
        h2 += h1;
        return {h1, h2};
    }

    uint64_t HashKey(const Value &value)
    {
        std::string key;
        AppendGroupKey(value, &key);
        return Hash64(key);
    }

} // namespace sql::cluster
