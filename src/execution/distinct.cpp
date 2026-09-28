#include "execution/distinct.h"
#include "execution/evaluator.h"

namespace sql
{

    void Distinct::Open()
    {
        seen_.clear();
        child_->Open();
    }

    bool Distinct::Next(Tuple *tuple)
    {
        while (child_->Next(tuple))
        {
            // Rows are duplicates when every value is, as for GROUP BY
            std::string key;
            for (size_t i = 0; i < tuple->GetValueCount(); ++i)
                AppendGroupKey(tuple->GetValue(i), &key);
            if (seen_.insert(std::move(key)).second)
                return true;
        }
        return false;
    }

    void Distinct::Close()
    {
        child_->Close();
        seen_.clear();
    }

} // namespace sql
