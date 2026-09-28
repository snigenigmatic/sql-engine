#include "execution/join_util.h"
#include "execution/evaluator.h"
#include "execution/filter.h"
#include <stdexcept>

namespace sql
{

    Tuple JoinOutput::Concat(const Tuple &left, const Tuple &right)
    {
        std::vector<Value> values;
        values.reserve(left.GetValueCount() + right.GetValueCount());
        for (size_t i = 0; i < left.GetValueCount(); ++i)
            values.push_back(left.GetValue(i));
        for (size_t i = 0; i < right.GetValueCount(); ++i)
            values.push_back(right.GetValue(i));
        return Tuple(std::move(values));
    }

    bool JoinOutput::Holds(const Tuple &joined) const
    {
        auto resolve = [&](const ColumnExpression &col) -> Value
        {
            const int idx = FindColumnIndex(*context, col.name);
            if (idx < 0)
                throw std::runtime_error("Unknown column: " + col.name);
            return joined.GetValue(static_cast<size_t>(idx));
        };
        for (const Expression *condition : residual)
        {
            if (!IsTrue(EvaluateExpression(condition, resolve)))
                return false;
        }
        return true;
    }

    Tuple JoinOutput::PadRight(const Tuple &left) const
    {
        std::vector<Value> values;
        values.reserve(left.GetValueCount() + right_schema->GetColumnCount());
        for (size_t i = 0; i < left.GetValueCount(); ++i)
            values.push_back(left.GetValue(i));
        for (const auto &column : right_schema->GetColumns())
            values.push_back(Value(column.type));
        return Tuple(std::move(values));
    }

} // namespace sql
