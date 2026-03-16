#include "storage/table.h"
#include <algorithm>

namespace sql
{

    void Table::Insert(const Tuple &tuple)
    {
        tuples_.push_back(tuple);
    }

    void Table::DeleteByIndices(std::vector<size_t> indices)
    {
        // Sort descending so we remove from the back first
        std::sort(indices.begin(), indices.end(), std::greater<size_t>());
        for (size_t idx : indices)
        {
            if (idx < tuples_.size())
            {
                tuples_.erase(tuples_.begin() + static_cast<long>(idx));
            }
        }
    }

    void Table::UpdateTuple(size_t index, const Tuple &tuple)
    {
        if (index < tuples_.size())
        {
            tuples_[index] = tuple;
        }
    }

    int Table::GetColumnIndex(const std::string &column_name) const
    {
        const auto &columns = schema_.GetColumns();
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (columns[i].name == column_name)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

} // namespace sql
