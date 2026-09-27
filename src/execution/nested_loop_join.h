#pragma once

#include "execution/join_util.h"
#include "execution/operator.h"
#include "storage/table.h"
#include <memory>
#include <vector>

namespace sql
{

    // Pairs every left row with every right row and keeps the pairs for
    // which the ON conditions hold (all pairs for a CROSS join). The right
    // input is read once and kept in memory.
    class NestedLoopJoin : public Operator
    {
    public:
        NestedLoopJoin(std::unique_ptr<Operator> left, Table *right, JoinOutput output)
            : left_(std::move(left)), right_(right), output_(std::move(output)) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        std::unique_ptr<Operator> left_;
        Table *right_;
        JoinOutput output_;

        std::vector<Tuple> right_rows_;
        Tuple left_row_;
        bool has_left_ = false;
        bool matched_ = false;
        size_t cursor_ = 0;
    };

} // namespace sql
