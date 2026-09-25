#include "execution/expression_projection.h"
#include "execution/evaluator.h"
#include "execution/filter.h"
#include <stdexcept>

namespace sql
{

    void ExpressionProjection::Open()
    {
        if (child_)
            child_->Open();
    }

    bool ExpressionProjection::Next(Tuple *tuple)
    {
        Tuple input;
        if (!child_ || !child_->Next(&input))
            return false;

        auto resolve = [&](const ColumnExpression &col) -> Value
        {
            const int idx = FindColumnIndex(*context_, col.name);
            if (idx < 0)
                throw std::runtime_error("Unknown column: " + col.name);
            return input.GetValue(static_cast<size_t>(idx));
        };

        std::vector<Value> values;
        values.reserve(exprs_.size());
        for (const Expression *expr : exprs_)
            values.push_back(EvaluateExpression(expr, resolve));
        *tuple = Tuple(std::move(values));
        return true;
    }

    void ExpressionProjection::Close()
    {
        if (child_)
            child_->Close();
    }

} // namespace sql
