#include "execution/hash_join.h"
#include "execution/evaluator.h"
#include <functional>
#include <stdexcept>
#include <utility>

namespace sql
{
    size_t HashJoin::JoinKeyHasher::operator()(const JoinKey &key) const
    {
        size_t seed = std::hash<int>{}(static_cast<int>(key.type));
        seed ^= std::hash<bool>{}(key.is_null) + 0x9e3779b9 + (seed << 6U) + (seed >> 2U);
        if (key.is_null)
        {
            return seed;
        }

        size_t payload_hash = 0;
        switch (key.type)
        {
        case DataType::INTEGER:
            payload_hash = std::hash<int32_t>{}(key.int_value);
            break;
        case DataType::FLOAT:
            payload_hash = std::hash<double>{}(key.float_value);
            break;
        case DataType::BOOLEAN:
            payload_hash = std::hash<bool>{}(key.bool_value);
            break;
        case DataType::VARCHAR:
            payload_hash = std::hash<std::string>{}(key.string_value);
            break;
        default:
            break;
        }
        seed ^= payload_hash + 0x9e3779b9 + (seed << 6U) + (seed >> 2U);
        return seed;
    }

    HashJoin::JoinKey HashJoin::MakeJoinKey(const Value &value)
    {
        JoinKey key;
        key.type = value.GetType();
        key.is_null = value.IsNull();
        if (key.is_null)
        {
            return key;
        }

        switch (key.type)
        {
        // Numbers are keyed by value so that 5 and 5.0 meet in one bucket
        case DataType::INTEGER:
            key.type = DataType::FLOAT;
            key.float_value = static_cast<double>(value.GetAsInt());
            break;
        case DataType::FLOAT:
            key.float_value = value.GetAsFloat();
            break;
        case DataType::BOOLEAN:
            key.bool_value = value.GetAsBool();
            break;
        case DataType::VARCHAR:
            key.string_value = value.GetAsString();
            break;
        default:
            break;
        }
        return key;
    }

    HashJoin::HashJoin(std::unique_ptr<Operator> left, Table *right, size_t left_key, size_t right_key,
                       JoinOutput output, bool build_right)
        : left_(std::move(left)), right_(right), left_key_(left_key), right_key_(right_key),
          output_(std::move(output)), build_right_(build_right)
    {
        if (!build_right_ && output_.type == JoinType::LEFT)
            throw std::logic_error("A LEFT hash join must build on the right");
    }

    void HashJoin::Open()
    {
        if (!left_ || right_ == nullptr)
            throw std::runtime_error("HashJoin requires both inputs");

        hash_table_.clear();
        build_rows_.clear();
        probe_rows_.clear();
        probe_cursor_ = 0;
        probe_valid_ = false;
        current_matches_ = nullptr;

        auto add_build_row = [&](const Tuple &row, size_t key)
        {
            // NULL keys can never match, so they are not worth hashing
            if (row.GetValue(key).IsNull())
                return;
            hash_table_[MakeJoinKey(row.GetValue(key))].push_back(build_rows_.size());
            build_rows_.push_back(row);
        };

        left_->Open();
        if (build_right_)
        {
            for (auto it = right_->begin(); it != right_->end(); ++it)
                add_build_row(*it, right_key_);
        }
        else
        {
            Tuple row;
            while (left_->Next(&row))
                add_build_row(row, left_key_);
            for (auto it = right_->begin(); it != right_->end(); ++it)
                probe_rows_.push_back(*it);
        }
    }

    bool HashJoin::NextProbe(Tuple *row)
    {
        if (build_right_)
            return left_->Next(row);
        if (probe_cursor_ >= probe_rows_.size())
            return false;
        *row = probe_rows_[probe_cursor_++];
        return true;
    }

    bool HashJoin::Next(Tuple *tuple)
    {
        const size_t probe_key = build_right_ ? left_key_ : right_key_;
        const size_t build_key = build_right_ ? right_key_ : left_key_;
        static const std::vector<size_t> no_matches;

        while (true)
        {
            if (!probe_valid_)
            {
                if (!NextProbe(&probe_tuple_))
                    return false;
                probe_valid_ = true;
                matched_ = false;
                match_cursor_ = 0;
                current_matches_ = &no_matches;
                const Value &key = probe_tuple_.GetValue(probe_key);
                if (!key.IsNull())
                {
                    auto it = hash_table_.find(MakeJoinKey(key));
                    if (it != hash_table_.end())
                        current_matches_ = &it->second;
                }
            }

            while (match_cursor_ < current_matches_->size())
            {
                const Tuple &build_tuple = build_rows_[(*current_matches_)[match_cursor_++]];
                // Same equality as WHERE: INTEGER and FLOAT compare numerically
                if (!IsTrue(EvaluateBinaryOp(TokenType::EQ, probe_tuple_.GetValue(probe_key),
                                             build_tuple.GetValue(build_key))))
                    continue;
                Tuple joined = build_right_ ? JoinOutput::Concat(probe_tuple_, build_tuple)
                                            : JoinOutput::Concat(build_tuple, probe_tuple_);
                if (!output_.Holds(joined))
                    continue;
                matched_ = true;
                *tuple = std::move(joined);
                return true;
            }

            probe_valid_ = false;
            if (output_.type == JoinType::LEFT && !matched_)
            {
                *tuple = output_.PadRight(probe_tuple_);
                return true;
            }
        }
    }

    void HashJoin::Close()
    {
        left_->Close();
        hash_table_.clear();
        build_rows_.clear();
        probe_rows_.clear();
        probe_valid_ = false;
        current_matches_ = nullptr;
    }

} // namespace sql
