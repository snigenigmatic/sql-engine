#pragma once

#include "execution/operator.h"
#include "storage/table.h"
#include <memory>
#include <vector>
#include <string>

namespace sql
{

    class Projection : public Operator
    {
    public:
        // If column_indices is empty and select_star is true, pass all columns
        Projection(std::unique_ptr<Operator> child, std::vector<int> column_indices, bool select_star = false)
            : child_(std::move(child)), column_indices_(std::move(column_indices)), select_star_(select_star) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        std::unique_ptr<Operator> child_;
        std::vector<int> column_indices_; // Which columns to project
        bool select_star_;
    };

} // namespace sql
