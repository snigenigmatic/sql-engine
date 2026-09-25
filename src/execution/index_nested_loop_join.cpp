#include "execution/index_nested_loop_join.h"
#include <stdexcept>

namespace sql
{

    namespace
    {
        std::string StripQualifier(const std::string &name)
        {
            size_t dot = name.find('.');
            if (dot == std::string::npos)
                return name;
            return name.substr(dot + 1);
        }
    } // namespace

    void IndexNestedLoopJoin::Open()
    {
        if (outer_table_ == nullptr || inner_table_ == nullptr || inner_index_ == nullptr)
            throw std::runtime_error("IndexNestedLoopJoin: null table or index");

        outer_col_idx_ = outer_table_->GetColumnIndex(StripQualifier(outer_col_));
        if (outer_col_idx_ < 0)
            throw std::runtime_error("IndexNestedLoopJoin: unknown outer column: " + outer_col_);

        outer_started_ = false;
        inner_matches_.clear();
        inner_cursor_ = 0;
    }

    bool IndexNestedLoopJoin::Next(Tuple *tuple)
    {
        while (true)
        {
            // Consume remaining inner matches for current outer row
            while (inner_cursor_ < inner_matches_.size())
            {
                Tuple inner_row;
                if (!inner_table_->GetTuple(inner_matches_[inner_cursor_++], &inner_row))
                    continue;

                std::vector<Value> joined;
                const Tuple &left_row = outer_is_left_ ? current_outer_ : inner_row;
                const Tuple &right_row = outer_is_left_ ? inner_row : current_outer_;
                joined.reserve(left_row.GetValueCount() + right_row.GetValueCount());
                for (size_t i = 0; i < left_row.GetValueCount(); ++i)
                    joined.push_back(left_row.GetValue(i));
                for (size_t i = 0; i < right_row.GetValueCount(); ++i)
                    joined.push_back(right_row.GetValue(i));

                *tuple = Tuple(std::move(joined));
                return true;
            }

            // Advance to next outer row
            if (!outer_started_)
            {
                outer_it_ = outer_table_->begin();
                outer_started_ = true;
            }
            else if (outer_it_ != outer_table_->end())
            {
                ++outer_it_;
            }
            if (outer_it_ == outer_table_->end())
                return false;

            current_outer_ = *outer_it_;
            Value probe_key = current_outer_.GetValue(static_cast<size_t>(outer_col_idx_));
            inner_matches_ = inner_index_->Search(probe_key);
            inner_cursor_ = 0;
        }
    }

    void IndexNestedLoopJoin::Close()
    {
        outer_it_ = TableIterator();
        outer_started_ = false;
        inner_matches_.clear();
        inner_cursor_ = 0;
        outer_col_idx_ = -1;
    }

} // namespace sql
