#pragma once

#include "execution/operator.h"
#include <memory>
#include <string>
#include <unordered_set>

namespace sql
{

    // SELECT DISTINCT: passes each distinct row through once, the first time
    // it appears (so an ORDER BY below it is preserved). NULLs count as equal.
    class Distinct : public Operator
    {
    public:
        explicit Distinct(std::unique_ptr<Operator> child) : child_(std::move(child)) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        std::unique_ptr<Operator> child_;
        std::unordered_set<std::string> seen_; // encoded rows
    };

} // namespace sql
