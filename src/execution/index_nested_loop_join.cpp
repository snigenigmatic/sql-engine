#include "execution/index_nested_loop_join.h"
#include "execution/evaluator.h"
#include <stdexcept>

namespace sql
{

    void IndexNestedLoopJoin::Open()
    {
        if (!outer_ || inner_table_ == nullptr || inner_index_ == nullptr)
            throw std::runtime_error("IndexNestedLoopJoin: null input or index");
        if (!outer_is_left_ && output_.type == JoinType::LEFT)
            throw std::logic_error("A LEFT index join must probe the right table");
        has_outer_ = false;
        inner_matches_.clear();
        inner_cursor_ = 0;
        outer_->Open();
    }

    bool IndexNestedLoopJoin::Next(Tuple *tuple)
    {
        while (true)
        {
            while (inner_cursor_ < inner_matches_.size())
            {
                Tuple inner_row;
                if (!inner_table_->GetTuple(inner_matches_[inner_cursor_++], &inner_row))
                    continue;
                Tuple joined = outer_is_left_ ? JoinOutput::Concat(current_outer_, inner_row)
                                              : JoinOutput::Concat(inner_row, current_outer_);
                if (!output_.Holds(joined))
                    continue;
                matched_ = true;
                *tuple = std::move(joined);
                return true;
            }

            if (has_outer_ && output_.type == JoinType::LEFT && !matched_)
            {
                has_outer_ = false;
                *tuple = output_.PadRight(current_outer_);
                return true;
            }

            if (!outer_->Next(&current_outer_))
                return false;
            has_outer_ = true;
            matched_ = false;
            const Value &probe_key = current_outer_.GetValue(outer_key_);
            // NULL never matches (and is not indexed). A number is looked up
            // as the indexed column's type, so 5 finds 5.0; 5.5 cannot equal
            // any INTEGER.
            std::optional<Value> key;
            if (!probe_key.IsNull())
                key = ConvertNumber(probe_key, inner_key_type_);
            if (key)
                inner_matches_ = inner_index_->Search(*key);
            else
                inner_matches_.clear();
            inner_cursor_ = 0;
        }
    }

    void IndexNestedLoopJoin::Close()
    {
        outer_->Close();
        has_outer_ = false;
        inner_matches_.clear();
        inner_cursor_ = 0;
    }

} // namespace sql
