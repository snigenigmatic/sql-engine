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
        const Schema &GetSchema() const { return schema_; }
        const std::string &GetName() const { return name_; }
        size_t GetTupleCount() const { return tuples_.size(); }
        int GetColumnIndex(const std::string &column_name) const;

        // Controlled access to individual tuples
        Tuple &GetTuple(size_t index) { return tuples_.at(index); }
        const Tuple &GetTuple(size_t index) const { return tuples_.at(index); }

        // Iterators for traversing tuples without exposing container internals
        std::vector<Tuple>::iterator begin() { return tuples_.begin(); }
        std::vector<Tuple>::iterator end() { return tuples_.end(); }
        std::vector<Tuple>::const_iterator begin() const { return tuples_.cbegin(); }
        std::vector<Tuple>::const_iterator end() const { return tuples_.cend(); }

    private:
        std::string name_;
        Schema schema_;
        std::vector<Tuple> tuples_;
    };

} // namespace sql
