#include "execution/filter.h"
#include "execution/evaluator.h"
#include <stdexcept>

namespace sql
{
    namespace
    {
        std::string StripQualifier(const std::string &name)
        {
            size_t dot = name.find('.');
            if (dot == std::string::npos)
            {
                return name;
            }
            return name.substr(dot + 1);
        }

        std::string ExtractQualifier(const std::string &name)
        {
            size_t dot = name.find('.');
            if (dot == std::string::npos)
            {
                return "";
            }
            return name.substr(0, dot);
        }
    } // namespace

    void Filter::Open()
    {
        if (child_)
        {
            child_->Open();
        }
    }

    bool Filter::Next(Tuple *tuple)
    {
        if (!child_)
        {
            return false;
        }

        // Keep fetching tuples until we find one that satisfies the predicate
        while (child_->Next(tuple))
        {
            if (predicate_ == nullptr || EvaluatePredicate(*tuple))
            {
                return true;
            }
        }
        return false;
    }

    void Filter::Close()
    {
        if (child_)
        {
            child_->Close();
        }
    }

    int FindColumnIndex(const Table &table, const std::string &name)
    {
        int idx = table.GetColumnIndex(name);
        if (idx < 0)
        {
            const std::string stripped = StripQualifier(name);
            const std::string qualifier = ExtractQualifier(name);
            int matched_idx = -1;
            const auto &columns = table.GetSchema().GetColumns();
            for (size_t i = 0; i < columns.size(); ++i)
            {
                const auto &schema_col = columns[i].name;
                const std::string schema_stripped = StripQualifier(schema_col);
                if (schema_stripped != stripped)
                {
                    continue;
                }
                if (!qualifier.empty() && ExtractQualifier(schema_col) != qualifier)
                {
                    const std::string schema_qualifier = ExtractQualifier(schema_col);
                    if (!schema_qualifier.empty())
                    {
                        if (schema_qualifier != qualifier)
                        {
                            continue;
                        }
                    }
                    else if (qualifier != table.GetName())
                    {
                        continue;
                    }
                }
                if (matched_idx >= 0)
                {
                    throw std::runtime_error("Ambiguous column: " + name);
                }
                matched_idx = static_cast<int>(i);
            }
            idx = matched_idx;
        }
        return idx;
    }

    Value Filter::ResolveColumn(const ColumnExpression &col, const Tuple &tuple) const
    {
        const int idx = FindColumnIndex(*table_, col.name);
        if (idx < 0)
        {
            throw std::runtime_error("Unknown column: " + col.name);
        }
        return tuple.GetValue(static_cast<size_t>(idx));
    }

    Value Filter::Evaluate(const Expression *expr, const Tuple &tuple) const
    {
        return EvaluateExpression(expr, [&](const ColumnExpression &col)
                                  { return ResolveColumn(col, tuple); });
    }

    bool Filter::EvaluatePredicate(const Tuple &tuple) const
    {
        if (!predicate_)
        {
            return true; // No predicate means all tuples pass
        }
        return IsTrue(Evaluate(predicate_, tuple));
    }

} // namespace sql
