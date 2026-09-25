#include "execution/seq_scan.h"

namespace sql
{

    void SeqScan::Open()
    {
        started_ = false;
    }

    bool SeqScan::Next(Tuple *tuple)
    {
        if (table_ == nullptr)
        {
            return false;
        }

        if (!started_)
        {
            it_ = table_->begin();
            started_ = true;
        }
        else if (it_ != table_->end())
        {
            ++it_;
        }

        if (it_ == table_->end())
        {
            return false;
        }

        *tuple = *it_;
        return true;
    }

    void SeqScan::Close()
    {
        it_ = TableIterator();
        started_ = false;
    }

} // namespace sql
