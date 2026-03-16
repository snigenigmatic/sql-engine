#include "execution/index_scan.h"

namespace sql
{

    void IndexScan::Open()
    {
        cursor_ = 0;
        if (is_point_)
            matching_rows_ = index_->Search(point_key_);
        else
            matching_rows_ = index_->RangeScan(low_, low_inclusive_, high_, high_inclusive_);
    }

    bool IndexScan::Next(Tuple *tuple)
    {
        const auto &tuples = table_->GetTuples();
        while (cursor_ < matching_rows_.size())
        {
            size_t row_idx = matching_rows_[cursor_++];
            if (row_idx < tuples.size())
            {
                *tuple = tuples[row_idx];
                return true;
            }
        }
        return false;
    }

    void IndexScan::Close()
    {
        matching_rows_.clear();
        cursor_ = 0;
    }

} // namespace sql
