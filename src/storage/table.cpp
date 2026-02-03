#include "storage/table.h"

namespace sql
{

    void Table::Insert(const Tuple &tuple)
    {
        tuples_.push_back(tuple);
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
