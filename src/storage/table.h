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

        // Insert a tuple into the table
        void Insert(const Tuple &tuple);

        // Get all tuples (for sequential scan)
        const std::vector<Tuple> &GetTuples() const { return tuples_; }

        // Get the schema
        const Schema &GetSchema() const { return schema_; }

        // Get table name
        const std::string &GetName() const { return name_; }

        // Get tuple count
        size_t GetTupleCount() const { return tuples_.size(); }

        // Get column index by name (-1 if not found)
        int GetColumnIndex(const std::string &column_name) const;

    private:
        std::string name_;
        Schema schema_;
        std::vector<Tuple> tuples_;
    };

} // namespace sql
