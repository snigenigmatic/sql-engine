#pragma once

#include "execution/operator.h"
#include "parser/ast.h"
#include "storage/table.h"
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sql
{

    // GROUP BY / aggregates: reads every input row (shaped like `context`),
    // groups rows whose key values are equal (NULLs together, 1 = 1.0) and
    // returns one row per group: the key values, then each aggregate's
    // result. Groups come out in the order they were first seen. Without
    // group keys the whole input is one group, so even empty input gives a
    // row (COUNT 0, other aggregates NULL).
    class HashAggregate : public Operator
    {
    public:
        HashAggregate(std::unique_ptr<Operator> child, std::vector<const Expression *> group_keys,
                      std::vector<const AggregateExpression *> aggregates, Table *context)
            : child_(std::move(child)), group_keys_(std::move(group_keys)), aggregates_(std::move(aggregates)),
              context_(context) {}

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        struct State
        {
            int64_t count = 0;    // rows (COUNT(*)) or non-NULL values
            int64_t int_sum = 0;  // INTEGER inputs of SUM / AVG
            double float_sum = 0; // FLOAT inputs of SUM / AVG
            bool has_float = false;
            std::optional<Value> best;           // MIN / MAX so far
            std::unordered_set<std::string> seen; // DISTINCT values so far
        };
        struct Group
        {
            std::vector<Value> keys;
            std::vector<State> states; // one per aggregate
        };

        void Accumulate(const AggregateExpression &aggregate, const Value &value, State *state) const;
        Value Finish(const AggregateExpression &aggregate, const State &state) const;

        std::unique_ptr<Operator> child_;
        std::vector<const Expression *> group_keys_;          // not owned
        std::vector<const AggregateExpression *> aggregates_; // not owned
        Table *context_;

        std::vector<Group> groups_;
        std::unordered_map<std::string, size_t> group_index_;
        size_t cursor_ = 0;
    };

} // namespace sql
