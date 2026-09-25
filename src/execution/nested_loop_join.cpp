#include "execution/nested_loop_join.h"
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
    } // namespace

    void NestedLoopJoin::Open()
    {
        if (left_table_ == nullptr || right_table_ == nullptr)
        {
            throw std::runtime_error("Join requires both left and right tables");
        }

        left_column_index_ = left_table_->GetColumnIndex(StripQualifier(left_column_));
        right_column_index_ = right_table_->GetColumnIndex(StripQualifier(right_column_));

        if (left_column_index_ < 0)
        {
            throw std::runtime_error("Unknown join column on left table: " + left_column_);
        }
        if (right_column_index_ < 0)
        {
            throw std::runtime_error("Unknown join column on right table: " + right_column_);
        }

        outer_started_ = false;
        inner_active_ = false;
    }

    bool NestedLoopJoin::Next(Tuple *tuple)
    {
        Table *outer_table = right_as_outer_ ? right_table_ : left_table_;
        Table *inner_table = right_as_outer_ ? left_table_ : right_table_;
        const auto outer_index = static_cast<size_t>(right_as_outer_ ? right_column_index_ : left_column_index_);
        const auto inner_index = static_cast<size_t>(right_as_outer_ ? left_column_index_ : right_column_index_);

        while (true)
        {
            if (!inner_active_)
            {
                // Advance to the next outer row and restart the inner scan
                if (!outer_started_)
                {
                    outer_it_ = outer_table->begin();
                    outer_started_ = true;
                }
                else
                {
                    ++outer_it_;
                }
                if (outer_it_ == outer_table->end())
                {
                    return false;
                }
                current_outer_ = *outer_it_;
                inner_it_ = inner_table->begin();
                inner_active_ = true;
            }

            const Value &outer_key = current_outer_.GetValue(outer_index);
            while (inner_it_ != inner_table->end())
            {
                const Tuple inner_row = *inner_it_;
                ++inner_it_;
                const Value &inner_key = inner_row.GetValue(inner_index);

                if (outer_key.GetType() != inner_key.GetType())
                {
                    continue;
                }

                if (outer_key == inner_key)
                {
                    std::vector<Value> joined_values;
                    const auto &left_row = right_as_outer_ ? inner_row : current_outer_;
                    const auto &right_row = right_as_outer_ ? current_outer_ : inner_row;
                    joined_values.reserve(left_row.GetValueCount() + right_row.GetValueCount());

                    for (size_t i = 0; i < left_row.GetValueCount(); ++i)
                    {
                        joined_values.push_back(left_row.GetValue(i));
                    }
                    for (size_t i = 0; i < right_row.GetValueCount(); ++i)
                    {
                        joined_values.push_back(right_row.GetValue(i));
                    }

                    *tuple = Tuple(std::move(joined_values));
                    return true;
                }
            }

            inner_active_ = false;
        }
    }

    void NestedLoopJoin::Close()
    {
        outer_it_ = TableIterator();
        inner_it_ = TableIterator();
        outer_started_ = false;
        inner_active_ = false;
        left_column_index_ = -1;
        right_column_index_ = -1;
    }

} // namespace sql
