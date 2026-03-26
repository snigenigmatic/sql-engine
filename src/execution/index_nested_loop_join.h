#pragma once

#include "execution/operator.h"
#include "storage/table.h"
#include "storage/btree.h"
#include <string>
#include <vector>

namespace sql
{

    // Index Nested-Loop Join: for each outer row, probes inner table via BTree index.
    // Output column order is always (left_table_cols..., right_table_cols...) regardless
    // of which side is outer; outer_is_left controls concatenation order.
    class IndexNestedLoopJoin : public Operator
    {
    public:
        // outer_table:   table iterated row by row
        // inner_table:   table probed via index
        // inner_index:   BTree on inner_table.inner_col
        // outer_col:     column name in outer_table used as probe key
        // inner_col:     indexed column name in inner_table
        // outer_is_left: if true, output is (outer || inner); else (inner || outer)
        IndexNestedLoopJoin(Table *outer_table, Table *inner_table,
                            BTree *inner_index,
                            std::string outer_col, std::string inner_col,
                            bool outer_is_left)
            : outer_table_(outer_table), inner_table_(inner_table),
              inner_index_(inner_index),
              outer_col_(std::move(outer_col)), inner_col_(std::move(inner_col)),
              outer_is_left_(outer_is_left) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        Table *outer_table_;
        Table *inner_table_;
        BTree *inner_index_;
        std::string outer_col_;
        std::string inner_col_;
        bool outer_is_left_;

        int outer_col_idx_ = -1;
        size_t outer_cursor_ = 0;
        Tuple current_outer_;
        std::vector<size_t> inner_matches_;
        size_t inner_cursor_ = 0;
    };

} // namespace sql
