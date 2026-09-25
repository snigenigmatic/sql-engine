#include "common/tuple.h"
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace sql
{

    std::string Tuple::ToString() const
    {
        std::stringstream ss;
        ss << "(";
        for (size_t i = 0; i < values_.size(); ++i)
        {
            if (i > 0)
            {
                ss << ", ";
            }
            ss << values_[i].ToString();
        }
        ss << ")";
        return ss.str();
    }

    void Tuple::SerializeTo(std::string *out) const
    {
        if (values_.size() > UINT16_MAX)
            throw std::runtime_error("Tuple has too many values to serialize");
        uint16_t count = static_cast<uint16_t>(values_.size());
        out->append(reinterpret_cast<const char *>(&count), sizeof(count));
        for (const auto &value : values_)
            value.SerializeTo(out);
    }

    bool Tuple::DeserializeFrom(const char *data, size_t size, Tuple *out)
    {
        const char *cursor = data;
        const char *end = data + size;
        uint16_t count;
        if (size < sizeof(count))
            return false;
        std::memcpy(&count, cursor, sizeof(count));
        cursor += sizeof(count);

        std::vector<Value> values;
        values.reserve(count);
        for (uint16_t i = 0; i < count; ++i)
        {
            Value v;
            if (!Value::DeserializeFrom(&cursor, end, &v))
                return false;
            values.push_back(std::move(v));
        }
        if (cursor != end)
            return false;
        *out = Tuple(std::move(values));
        return true;
    }

} // namespace sql
