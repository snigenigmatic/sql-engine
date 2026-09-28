#include "execution/sort.h"
#include "execution/evaluator.h"
#include "execution/filter.h"
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace sql
{

    void Sort::Open()
    {
        rows_.clear();
        cursor_ = 0;
        child_->Open();

        // Evaluate every key once per row, then sort row positions by them
        std::vector<std::vector<Value>> keys;
        Tuple row;
        while (child_->Next(&row))
        {
            auto resolve = [&](const ColumnExpression &col) -> Value
            {
                const int idx = FindColumnIndex(*context_, col.name);
                if (idx < 0)
                    throw std::runtime_error("Unknown column: " + col.name);
                return row.GetValue(static_cast<size_t>(idx));
            };
            std::vector<Value> row_keys;
            row_keys.reserve(keys_.size());
            for (const Expression *key : keys_)
                row_keys.push_back(EvaluateExpression(key, resolve));
            keys.push_back(std::move(row_keys));
            rows_.push_back(row);
        }
        child_->Close();

        std::vector<size_t> order(rows_.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b)
                         {
            for (size_t k = 0; k < keys_.size(); ++k)
            {
                const int c = CompareForSort(keys[a][k], keys[b][k]);
                if (c != 0)
                    return descending_[k] ? c > 0 : c < 0;
            }
            return false; });

        std::vector<Tuple> sorted;
        sorted.reserve(rows_.size());
        for (size_t i : order)
            sorted.push_back(std::move(rows_[i]));
        rows_ = std::move(sorted);
    }

    bool Sort::Next(Tuple *tuple)
    {
        if (cursor_ >= rows_.size())
            return false;
        *tuple = rows_[cursor_++];
        return true;
    }

    void Sort::Close()
    {
        rows_.clear();
        cursor_ = 0;
    }

} // namespace sql
