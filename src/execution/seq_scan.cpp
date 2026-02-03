#include "execution/seq_scan.h"

namespace sql
{

    void SeqScan::Open()
    {
        cursor_ = 0;
    }

    bool SeqScan::Next(Tuple *tuple)
    {
        if (table_ == nullptr)
        {
            return false;
        }

        const auto &tuples = table_->GetTuples();
        if (cursor_ >= tuples.size())
        {
            return false;
        }

        *tuple = tuples[cursor_];
        cursor_++;
        return true;
    }

    void SeqScan::Close()
    {
        cursor_ = 0;
    }

} // namespace sql
