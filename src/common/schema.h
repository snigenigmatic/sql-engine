#pragma once

#include "common/types.h"
#include <vector>
#include <string>

namespace sql
{

    struct Column
    {
        std::string name;
        DataType type;
        int length = 0;

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
