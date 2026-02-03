#pragma once

#include "common/value.h"
#include "common/schema.h"
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
        std::string ToString() const;

    private:
        std::vector<Value> values_;
        Schema *schema_ = nullptr;
    };

} // namespace sql
