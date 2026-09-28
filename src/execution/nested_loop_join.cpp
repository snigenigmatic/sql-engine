#include "execution/nested_loop_join.h"
#include <stdexcept>

namespace sql
{

    void NestedLoopJoin::Open()
    {
        if (!left_ || right_ == nullptr)
            throw std::runtime_error("Join requires both inputs");
        right_rows_.clear();
        for (auto it = right_->begin(); it != right_->end(); ++it)
            right_rows_.push_back(*it);
        has_left_ = false;
        left_->Open();
    }

    bool NestedLoopJoin::Next(Tuple *tuple)
    {
        while (true)
        {
            if (!has_left_)
            {
                if (!left_->Next(&left_row_))
                    return false;
                has_left_ = true;
                matched_ = false;
                cursor_ = 0;
            }
            while (cursor_ < right_rows_.size())
            {
                Tuple joined = JoinOutput::Concat(left_row_, right_rows_[cursor_++]);
                if (output_.Holds(joined))
                {
                    matched_ = true;
                    *tuple = std::move(joined);
                    return true;
                }
            }
            has_left_ = false;
            if (output_.type == JoinType::LEFT && !matched_)
            {
                *tuple = output_.PadRight(left_row_);
                return true;
            }
        }
    }

    void NestedLoopJoin::Close()
    {
        left_->Close();
        right_rows_.clear();
        has_left_ = false;
    }

} // namespace sql
