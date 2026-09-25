#pragma once

#include "common/types.h"
#include "common/value.h"
#include <optional>
#include <vector>
#include <string>

namespace sql
{

    struct Column
    {
        std::string name;
        DataType type;
        int length = 0; // VARCHAR(n) limit; 0 = unlimited

        // Constraints
        bool not_null = false;
        bool primary_key = false; // implies not_null and unique
        bool unique = false;
        std::optional<Value> default_value; // used when INSERT omits the column

        Column(std::string n, DataType t, int l = 0)
            : name(std::move(n)), type(t), length(l) {}
    };

    class Schema
    {
    public:
        Schema() = default;
        explicit Schema(std::vector<Column> columns) : columns_(std::move(columns)) {}

        const std::vector<Column> &GetColumns() const { return columns_; }

        const Column &GetColumn(size_t index) const { return columns_.at(index); }

        size_t GetColumnCount() const { return columns_.size(); }

    private:
        std::vector<Column> columns_;
    };

} // namespace sql
