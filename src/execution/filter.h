#pragma once

#include "execution/operator.h"
#include "parser/ast.h"
#include "storage/table.h"
#include <memory>

namespace sql
{

    class Filter : public Operator
    {
    public:
        Filter(std::unique_ptr<Operator> child, Expression *predicate, Table *table)
            : child_(std::move(child)), predicate_(predicate), table_(table) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        // Evaluate an expression against a tuple, returns the resulting Value
        Value Evaluate(const Expression *expr, const Tuple &tuple) const;

        // Check if the predicate is satisfied by the tuple
        bool EvaluatePredicate(const Tuple &tuple) const;

        std::unique_ptr<Operator> child_;
        Expression *predicate_; // The WHERE expression (not owned)
        Table *table_;          // For schema access (column name lookups)
    };

} // namespace sql
