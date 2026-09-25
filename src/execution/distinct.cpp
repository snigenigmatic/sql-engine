#include "execution/distinct.h"

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
            // The binary encoding identifies a row (type, NULL-ness, value)
            std::string key;
            tuple->SerializeTo(&key);
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
