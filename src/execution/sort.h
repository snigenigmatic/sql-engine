#pragma once

#include "execution/operator.h"
#include "parser/ast.h"
#include "storage/table.h"
#include <memory>
#include <vector>

namespace sql
{

    // ORDER BY: reads every input row, then returns them sorted by the keys
    // (evaluated against rows shaped like `context`). The sort is stable, so
    // rows with equal keys keep their input order.
    class Sort : public Operator
    {
    public:
        Sort(std::unique_ptr<Operator> child, std::vector<const Expression *> keys, std::vector<bool> descending,
             Table *context)
            : child_(std::move(child)), keys_(std::move(keys)), descending_(std::move(descending)), context_(context) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        std::unique_ptr<Operator> child_;
        std::vector<const Expression *> keys_; // not owned
        std::vector<bool> descending_;
        Table *context_;

        std::vector<Tuple> rows_;
        size_t cursor_ = 0;
    };

} // namespace sql
