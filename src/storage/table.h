#pragma once

#include "common/tuple.h"
#include "common/schema.h"
#include <vector>
#include <string>

namespace sql
{

    class Table
    {
    public:
        Table() = default;
        explicit Table(std::string name, Schema schema)
            : name_(std::move(name)), schema_(std::move(schema)) {}

        void Insert(const Tuple &tuple);

        // Delete tuples by indices (sorted descending to avoid shifting issues)
        void DeleteByIndices(std::vector<size_t> indices);

        // Update a tuple at a given index
        void UpdateTuple(size_t index, const Tuple &tuple);

        const std::vector<Tuple> &GetTuples() const { return tuples_; }
        std::vector<Tuple> &GetMutableTuples() { return tuples_; }
        const Schema &GetSchema() const { return schema_; }
        const std::string &GetName() const { return name_; }
        size_t GetTupleCount() const { return tuples_.size(); }
        int GetColumnIndex(const std::string &column_name) const;

    private:
        std::string name_;
        Schema schema_;
        std::vector<Tuple> tuples_;
    };

} // namespace sql
