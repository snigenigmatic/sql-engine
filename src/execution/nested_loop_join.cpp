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

        left_cursor_ = 0;
        right_cursor_ = 0;
    }

    bool NestedLoopJoin::Next(Tuple *tuple)
    {
        const auto &left_rows = left_table_->GetTuples();
        const auto &right_rows = right_table_->GetTuples();

        while ((!right_as_outer_ && left_cursor_ < left_rows.size()) ||
               (right_as_outer_ && right_cursor_ < right_rows.size()))
        {
            const auto &outer_row = right_as_outer_ ? right_rows[right_cursor_] : left_rows[left_cursor_];
            Value outer_key = right_as_outer_
                                  ? outer_row.GetValue(static_cast<size_t>(right_column_index_))
                                  : outer_row.GetValue(static_cast<size_t>(left_column_index_));

            const size_t inner_size = right_as_outer_ ? left_rows.size() : right_rows.size();
            while ((right_as_outer_ ? left_cursor_ : right_cursor_) < inner_size)
            {
                const auto &inner_row = right_as_outer_ ? left_rows[left_cursor_++] : right_rows[right_cursor_++];
                Value inner_key = right_as_outer_
                                      ? inner_row.GetValue(static_cast<size_t>(left_column_index_))
                                      : inner_row.GetValue(static_cast<size_t>(right_column_index_));

                if (outer_key.GetType() != inner_key.GetType())
                {
                    continue;
                }

                if (outer_key == inner_key)
                {
                    std::vector<Value> joined_values;
                    const auto &left_row = right_as_outer_ ? inner_row : outer_row;
                    const auto &right_row = right_as_outer_ ? outer_row : inner_row;
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

            if (right_as_outer_)
            {
                ++right_cursor_;
                left_cursor_ = 0;
            }
            else
            {
                ++left_cursor_;
                right_cursor_ = 0;
            }
        }

        return false;
    }

    void NestedLoopJoin::Close()
    {
        left_cursor_ = 0;
        right_cursor_ = 0;
        left_column_index_ = -1;
        right_column_index_ = -1;
    }

} // namespace sql
