#pragma once

#include "execution/operator.h"
#include "parser/ast.h"
#include "storage/table.h"
#include <memory>
#include <vector>

namespace sql
{

    // Computes each SELECT-list expression for every input row
    // (SELECT price * qty AS total, name ...). Column references are
    // resolved against `context`, a table shaped like the input rows: the
    // base table, or the qualified join schema after a join.
    class ExpressionProjection : public Operator
    {
    public:
        ExpressionProjection(std::unique_ptr<Operator> child, std::vector<const Expression *> exprs, Table *context)
            : child_(std::move(child)), exprs_(std::move(exprs)), context_(context) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        std::unique_ptr<Operator> child_;
        std::vector<const Expression *> exprs_; // not owned
        Table *context_;
    };

} // namespace sql
