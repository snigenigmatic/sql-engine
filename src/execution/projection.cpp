#include "execution/projection.h"

namespace sql
{

    void Projection::Open()
    {
        if (child_)
        {
            child_->Open();
        }
    }

    bool Projection::Next(Tuple *tuple)
    {
        if (!child_)
        {
            return false;
        }

        Tuple child_tuple;
        if (!child_->Next(&child_tuple))
        {
            return false;
        }

        // If SELECT *, return the entire tuple
        if (select_star_)
        {
            *tuple = child_tuple;
            return true;
        }

        // Project only the requested columns
        std::vector<Value> projected_values;
        projected_values.reserve(column_indices_.size());

        for (int idx : column_indices_)
        {
            projected_values.push_back(child_tuple.GetValue(static_cast<size_t>(idx)));
        }

        *tuple = Tuple(std::move(projected_values));
        return true;
    }

    void Projection::Close()
    {
        if (child_)
        {
            child_->Close();
        }
    }

} // namespace sql
