#include "execution/aggregate.h"
#include "execution/evaluator.h"
#include "execution/filter.h"
#include <climits>
#include <stdexcept>

namespace sql
{

    void HashAggregate::Open()
    {
        groups_.clear();
        group_index_.clear();
        cursor_ = 0;
        child_->Open();

        Tuple row;
        auto resolve = [&](const ColumnExpression &col) -> Value
        {
            const int idx = FindColumnIndex(*context_, col.name);
            if (idx < 0)
                throw std::runtime_error("Unknown column: " + col.name);
            return row.GetValue(static_cast<size_t>(idx));
        };

        while (child_->Next(&row))
        {
            std::vector<Value> keys;
            std::string encoded;
            keys.reserve(group_keys_.size());
            for (const Expression *key : group_keys_)
            {
                keys.push_back(EvaluateExpression(key, resolve));
                AppendGroupKey(keys.back(), &encoded);
            }

            auto [it, inserted] = group_index_.emplace(std::move(encoded), groups_.size());
            if (inserted)
                groups_.push_back(Group{std::move(keys), std::vector<State>(aggregates_.size())});
            Group &group = groups_[it->second];

            for (size_t i = 0; i < aggregates_.size(); ++i)
            {
                const AggregateExpression &aggregate = *aggregates_[i];
                if (!aggregate.argument)
                {
                    ++group.states[i].count; // COUNT(*)
                    continue;
                }
                Accumulate(aggregate, EvaluateExpression(aggregate.argument.get(), resolve), &group.states[i]);
            }
        }
        child_->Close();

        // Without GROUP BY there is always exactly one group
        if (group_keys_.empty() && groups_.empty())
            groups_.push_back(Group{{}, std::vector<State>(aggregates_.size())});
    }

    void HashAggregate::Accumulate(const AggregateExpression &aggregate, const Value &value, State *state) const
    {
        if (value.IsNull())
            return; // every aggregate ignores NULLs
        if (aggregate.distinct)
        {
            std::string key;
            AppendGroupKey(value, &key);
            if (!state->seen.insert(std::move(key)).second)
                return;
        }

        switch (aggregate.function)
        {
        case AggregateFunction::COUNT:
            ++state->count;
            break;
        case AggregateFunction::SUM:
        case AggregateFunction::AVG:
            if (value.GetType() == DataType::INTEGER)
            {
                if (__builtin_add_overflow(state->int_sum, static_cast<int64_t>(value.GetAsInt()), &state->int_sum))
                    throw std::runtime_error("Integer overflow");
            }
            else if (value.GetType() == DataType::FLOAT)
            {
                state->float_sum += value.GetAsFloat();
                state->has_float = true;
            }
            else
            {
                throw std::runtime_error(std::string(AggregateFunctionName(aggregate.function)) +
                                         " requires numeric values, got " + value.ToString());
            }
            ++state->count;
            break;
        case AggregateFunction::MIN:
        case AggregateFunction::MAX:
        {
            const bool better = !state->best ||
                                (aggregate.function == AggregateFunction::MIN ? CompareForSort(value, *state->best) < 0
                                                                              : CompareForSort(value, *state->best) > 0);
            if (better)
                state->best = value;
            ++state->count;
            break;
        }
        }
    }

    Value HashAggregate::Finish(const AggregateExpression &aggregate, const State &state) const
    {
        switch (aggregate.function)
        {
        case AggregateFunction::COUNT:
            if (state.count > INT32_MAX)
                throw std::runtime_error("Integer overflow");
            return Value(static_cast<int32_t>(state.count));
        case AggregateFunction::SUM:
            if (state.count == 0)
                return Value(DataType::INTEGER); // NULL: nothing to add up
            if (state.has_float)
                return Value(static_cast<double>(state.int_sum) + state.float_sum);
            if (state.int_sum < INT32_MIN || state.int_sum > INT32_MAX)
                throw std::runtime_error("Integer overflow");
            return Value(static_cast<int32_t>(state.int_sum));
        case AggregateFunction::AVG:
            if (state.count == 0)
                return Value(DataType::FLOAT);
            return Value((static_cast<double>(state.int_sum) + state.float_sum) / static_cast<double>(state.count));
        case AggregateFunction::MIN:
        case AggregateFunction::MAX:
            return state.best ? *state.best : Value();
        }
        throw std::logic_error("Unknown aggregate function");
    }

    bool HashAggregate::Next(Tuple *tuple)
    {
        if (cursor_ >= groups_.size())
            return false;
        const Group &group = groups_[cursor_++];
        std::vector<Value> values = group.keys;
        for (size_t i = 0; i < aggregates_.size(); ++i)
            values.push_back(Finish(*aggregates_[i], group.states[i]));
        *tuple = Tuple(std::move(values));
        return true;
    }

    void HashAggregate::Close()
    {
        groups_.clear();
        group_index_.clear();
        cursor_ = 0;
    }

} // namespace sql
