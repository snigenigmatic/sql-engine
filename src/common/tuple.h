#pragma once

#include "common/value.h"
#include "common/schema.h"
#include <string>
#include <vector>

namespace sql
{

    class Tuple
    {
    public:
        Tuple() = default;
        Tuple(std::vector<Value> values, Schema *schema = nullptr)
            : values_(std::move(values)), schema_(schema) {}

        const Value &GetValue(size_t index) const { return values_.at(index); }
        size_t GetValueCount() const { return values_.size(); }
        std::string ToString() const;

        // Binary encoding: u16 value count followed by each Value's encoding
        void SerializeTo(std::string *out) const;
        static bool DeserializeFrom(const char *data, size_t size, Tuple *out);

    private:
        std::vector<Value> values_;
        Schema *schema_ = nullptr;
    };

} // namespace sql
