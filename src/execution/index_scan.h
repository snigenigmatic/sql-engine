#pragma once

#include "execution/operator.h"
#include "storage/table.h"
#include "storage/btree.h"
#include <vector>
#include <optional>

namespace sql{
    class IndexScan : public Operator
    {
    public:
        // Point lookup
        IndexScan(Table *table, BTree *index, const Value &key)
            : table_(table), index_(index), point_key_(key), is_point_(true) {}

        // Range scan
        IndexScan(Table *table, BTree *index,
                  std::optional<Value> low, bool low_inclusive,
                  std::optional<Value> high, bool high_inclusive)
            : table_(table), index_(index),
              low_(std::move(low)), low_inclusive_(low_inclusive),
              high_(std::move(high)), high_inclusive_(high_inclusive),
              is_point_(false) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        Table *table_;
        BTree *index_;

        // Point lookup params
        Value point_key_;
        bool is_point_ = false;

        // Range scan params
        std::optional<Value> low_;
        bool low_inclusive_ = true;
        std::optional<Value> high_;
        bool high_inclusive_ = true;

        // Runtime state
        std::vector<size_t> matching_rows_;
        size_t cursor_ = 0;
    };
} // namespace sql