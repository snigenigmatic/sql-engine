#pragma once

#include "execution/join_util.h"
#include "execution/operator.h"
#include "storage/btree.h"
#include "storage/table.h"
#include <memory>
#include <vector>

namespace sql
{

    // Index nested-loop join: for each outer row, looks its key up in the
    // B+tree on the inner table's join column. Output rows are always
    // left || right: outer || inner when outer_is_left, else inner || outer
    // (an INNER join whose left table has the index).
    class IndexNestedLoopJoin : public Operator
    {
    public:
        IndexNestedLoopJoin(std::unique_ptr<Operator> outer, Table *inner_table, BTree *inner_index,
                            size_t outer_key, DataType inner_key_type, JoinOutput output, bool outer_is_left)
            : outer_(std::move(outer)), inner_table_(inner_table), inner_index_(inner_index), outer_key_(outer_key),
              inner_key_type_(inner_key_type), output_(std::move(output)), outer_is_left_(outer_is_left) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        std::unique_ptr<Operator> outer_;
        Table *inner_table_;
        BTree *inner_index_;
        size_t outer_key_;
        DataType inner_key_type_;
        JoinOutput output_;
        bool outer_is_left_;

        Tuple current_outer_;
        bool has_outer_ = false;
        bool matched_ = false;
        std::vector<RID> inner_matches_;
        size_t inner_cursor_ = 0;
    };

} // namespace sql
