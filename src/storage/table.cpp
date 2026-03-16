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
        if (indices.empty() || tuples_.empty())
            return;

        // Sort and remove duplicates to avoid deleting the same row multiple times
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

        // Rebuild vector in one pass (O(n) instead of O(k*n))
        std::vector<Tuple> new_tuples;
        const std::size_t max_deletions = std::min(tuples_.size(), indices.size());
        new_tuples.reserve(tuples_.size() - max_deletions);

        std::size_t delete_pos = 0;
        for (std::size_t i = 0; i < tuples_.size(); ++i)
        {
            // Skip tuple if its index matches the next delete index
            if (delete_pos < indices.size() && i == indices[delete_pos])
            {
                ++delete_pos;
                continue;
            }
            new_tuples.push_back(std::move(tuples_[i]));
        }
        tuples_ = std::move(new_tuples);
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
