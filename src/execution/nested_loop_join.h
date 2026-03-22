#pragma once

#include "execution/operator.h"
#include "storage/table.h"
#include <string>

namespace sql
{

    class NestedLoopJoin : public Operator
    {
    public:
        NestedLoopJoin(Table *left_table, Table *right_table, std::string left_column, std::string right_column, bool right_as_outer = false)
            : left_table_(left_table),
              right_table_(right_table),
              left_column_(std::move(left_column)),
              right_column_(std::move(right_column)),
              right_as_outer_(right_as_outer) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        Table *left_table_;
        Table *right_table_;
        std::string left_column_;
        std::string right_column_;

        int left_column_index_ = -1;
        int right_column_index_ = -1;
        size_t left_cursor_ = 0;
        size_t right_cursor_ = 0;
        bool right_as_outer_ = false;
    };

} // namespace sql
