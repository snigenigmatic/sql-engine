#include "execution/limit.h"

namespace sql
{

    void Limit::Open()
    {
        skipped_ = 0;
        returned_ = 0;
        child_->Open();
    }

    bool Limit::Next(Tuple *tuple)
    {
        if (limit_ && returned_ >= *limit_)
            return false; // done; no need to read further
        while (skipped_ < offset_)
        {
            if (!child_->Next(tuple))
                return false;
            ++skipped_;
        }
        if (!child_->Next(tuple))
            return false;
        ++returned_;
        return true;
    }

    void Limit::Close()
    {
        child_->Close();
    }

} // namespace sql
