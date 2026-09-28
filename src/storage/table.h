#pragma once

#include "common/tuple.h"
#include "common/schema.h"
#include "storage/rid.h"
#include "storage/table_heap.h"
#include <memory>
#include <optional>
#include <vector>
#include <string>

namespace sql
{

    class Table;

    // Forward iterator over (RID, Tuple) pairs. Holds a copy of the current
    // tuple, so it stays valid while other rows are modified.
    class TableIterator
    {
    public:
        TableIterator() = default;

        const Tuple &operator*() const { return tuple_; }
        const Tuple *operator->() const { return &tuple_; }
        const RID &GetRID() const { return rid_; }

        TableIterator &operator++();

        bool operator==(const TableIterator &other) const
        {
            return table_ == other.table_ && at_end_ == other.at_end_ && (at_end_ || rid_ == other.rid_);
        }
        bool operator!=(const TableIterator &other) const { return !(*this == other); }

    private:
        friend class Table;
        TableIterator(const Table *table, bool at_end) : table_(table), at_end_(at_end) {}

        const Table *table_ = nullptr;
        bool at_end_ = true;
        RID rid_;
        Tuple tuple_;
    };

    // A named relation. Catalog tables are backed by a TableHeap of slotted
    // pages; tables constructed without a heap are temporary in-memory tables
    // used for intermediate query results.
    class Table
    {
    public:
        Table() = default;

        // Temporary in-memory table
        Table(std::string name, Schema schema)
            : name_(std::move(name)), schema_(std::move(schema)) {}

        // Heap-backed table
        Table(std::string name, Schema schema, std::unique_ptr<TableHeap> heap);

        // Throws std::runtime_error if the row cannot be stored
        RID Insert(const Tuple &tuple);

        bool GetTuple(const RID &rid, Tuple *tuple) const;

        // Replaces the row at rid. The row may move; *new_rid (optional)
        // receives its final location.
        bool UpdateTuple(const RID &rid, const Tuple &tuple, RID *new_rid = nullptr);

        bool DeleteTuple(const RID &rid);

        // Release all storage (heap pages go back to the free list)
        void Drop();

        const Schema &GetSchema() const { return schema_; }
        const std::string &GetName() const { return name_; }
        size_t GetTupleCount() const { return tuple_count_; }
        int GetColumnIndex(const std::string &column_name) const;

        bool IsTemporary() const { return heap_ == nullptr; }
        TableHeap *GetHeap() const { return heap_.get(); }

        TableIterator begin() const;
        TableIterator end() const { return TableIterator(this, true); }

    private:
        friend class TableIterator;

        // Locate the first live row at or after the given position
        bool Advance(const RID *after, RID *rid, Tuple *tuple) const;

        std::string name_;
        Schema schema_;
        std::unique_ptr<TableHeap> heap_;
        size_t tuple_count_ = 0;

        // Temporary tables only. Deleted rows are left as empty optionals so
        // RIDs (row positions) stay stable.
        std::vector<std::optional<Tuple>> temp_rows_;
    };

} // namespace sql
