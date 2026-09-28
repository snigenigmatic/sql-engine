#pragma once

#include "execution/operator.h"
#include "storage/table.h"

namespace sql
{

    class SeqScan : public Operator
    {
    public:
        explicit SeqScan(Table *table) : table_(table) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

        // Get the underlying table
        Table *GetTable() const { return table_; }

    private:
        Table *table_;
        TableIterator it_;
        bool started_ = false;
    };

} // namespace sql
