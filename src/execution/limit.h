#pragma once

#include "execution/operator.h"
#include <cstdint>
#include <memory>
#include <optional>

namespace sql
{

    // LIMIT n OFFSET m: skips the first m rows, then returns at most n
    class Limit : public Operator
    {
    public:
        Limit(std::unique_ptr<Operator> child, std::optional<int64_t> limit, int64_t offset)
            : child_(std::move(child)), limit_(limit), offset_(offset) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        std::unique_ptr<Operator> child_;
        std::optional<int64_t> limit_;
        int64_t offset_;
        int64_t skipped_ = 0;
        int64_t returned_ = 0;
    };

} // namespace sql
