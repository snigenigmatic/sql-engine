#pragma once

#include "execution/operator.h"
#include "parser/ast.h"
#include "storage/table.h"
#include <memory>

namespace sql
{

    // Index of a column referenced by name in rows shaped like `table`:
    // an exact match, or else a unique match ignoring "table." qualifiers
    // (join rows use qualified names). Returns -1 if absent; throws if an
    // unqualified name matches more than one column.
    int FindColumnIndex(const Table &table, const std::string &name);

    class Filter : public Operator
    {
    public:
        Filter(std::unique_ptr<Operator> child, const Expression *predicate, Table *table)
            : child_(std::move(child)), predicate_(predicate), table_(table) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        // Evaluate an expression against a tuple, returns the resulting Value
        Value Evaluate(const Expression *expr, const Tuple &tuple) const;
        Value ResolveColumn(const ColumnExpression &col, const Tuple &tuple) const;

        // Check if the predicate is satisfied by the tuple
        bool EvaluatePredicate(const Tuple &tuple) const;

        std::unique_ptr<Operator> child_;
        const Expression *predicate_; // The WHERE expression (not owned)
        Table *table_;          // For schema access (column name lookups)
    };

} // namespace sql
