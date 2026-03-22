#include "execution/hash_join.h"
#include <functional>
#include <stdexcept>
#include <utility>

namespace sql
{
    namespace
    {
        std::string StripQualifier(const std::string &name)
        {
            size_t dot = name.find('.');
            if (dot == std::string::npos)
            {
                return name;
            }
            return name.substr(dot + 1);
        }
    } // namespace

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
        case DataType::INTEGER:
            key.int_value = value.GetAsInt();
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

    HashJoin::HashJoin(Table *left_table, Table *right_table, std::string left_column, std::string right_column, bool build_right)
        : left_table_(left_table), right_table_(right_table),
          left_column_(std::move(left_column)), right_column_(std::move(right_column)),
          build_right_(build_right)
    {
    }

    void HashJoin::Open()
    {
        if (left_table_ == nullptr || right_table_ == nullptr)
        {
            throw std::runtime_error("HashJoin requires valid input tables");
        }

        left_column_index_ = left_table_->GetColumnIndex(StripQualifier(left_column_));
        right_column_index_ = right_table_->GetColumnIndex(StripQualifier(right_column_));
        if (left_column_index_ < 0 || right_column_index_ < 0)
        {
            throw std::runtime_error("HashJoin column not found in schema");
        }

        const auto &left_rows = left_table_->GetTuples();
        const auto &right_rows = right_table_->GetTuples();

        const std::vector<Tuple> *build_rows = build_right_ ? &right_rows : &left_rows;
        const std::vector<Tuple> *probe_rows = build_right_ ? &left_rows : &right_rows;
        int build_index = build_right_ ? right_column_index_ : left_column_index_;

        hash_table_.clear();
        hash_table_.reserve(build_rows->size());
        for (size_t i = 0; i < build_rows->size(); ++i)
        {
            const Tuple &build_tuple = (*build_rows)[i];
            if (build_index < 0 || static_cast<size_t>(build_index) >= build_tuple.GetValueCount())
            {
                continue;
            }
            const Value &key = build_tuple.GetValue(static_cast<size_t>(build_index));
            hash_table_[MakeJoinKey(key)].push_back(i);
        }

        build_rows_ = build_rows;
        probe_rows_ = probe_rows;
        probe_is_left_ = build_right_;
        probe_cursor_ = 0;
        match_cursor_ = 0;
        current_matches_.clear();
    }

    bool HashJoin::Next(Tuple *tuple)
    {
        if (tuple == nullptr || probe_rows_ == nullptr || build_rows_ == nullptr)
        {
            return false;
        }

        const int probe_key_index = probe_is_left_ ? left_column_index_ : right_column_index_;
        const int build_key_index = probe_is_left_ ? right_column_index_ : left_column_index_;

        while (probe_cursor_ < probe_rows_->size())
        {
            const Tuple &probe_tuple = (*probe_rows_)[probe_cursor_];
            if (probe_key_index < 0 || static_cast<size_t>(probe_key_index) >= probe_tuple.GetValueCount())
            {
                ++probe_cursor_;
                match_cursor_ = 0;
                current_matches_.clear();
                continue;
            }

            if (current_matches_.empty() && match_cursor_ == 0)
            {
                const Value &probe_key = probe_tuple.GetValue(static_cast<size_t>(probe_key_index));
                auto it = hash_table_.find(MakeJoinKey(probe_key));
                if (it != hash_table_.end())
                {
                    current_matches_ = it->second;
                }
            }

            while (match_cursor_ < current_matches_.size())
            {
                const Tuple &build_tuple = (*build_rows_)[current_matches_[match_cursor_++]];
                if (build_key_index < 0 || static_cast<size_t>(build_key_index) >= build_tuple.GetValueCount())
                {
                    continue;
                }

                const Value &probe_key = probe_tuple.GetValue(static_cast<size_t>(probe_key_index));
                const Value &build_key = build_tuple.GetValue(static_cast<size_t>(build_key_index));
                if (probe_key.GetType() != build_key.GetType() || probe_key != build_key)
                {
                    continue;
                }

                std::vector<Value> joined_values;
                joined_values.reserve(probe_tuple.GetValueCount() + build_tuple.GetValueCount());
                if (probe_is_left_)
                {
                    for (size_t i = 0; i < probe_tuple.GetValueCount(); ++i)
                    {
                        joined_values.push_back(probe_tuple.GetValue(i));
                    }
                    for (size_t i = 0; i < build_tuple.GetValueCount(); ++i)
                    {
                        joined_values.push_back(build_tuple.GetValue(i));
                    }
                }
                else
                {
                    for (size_t i = 0; i < build_tuple.GetValueCount(); ++i)
                    {
                        joined_values.push_back(build_tuple.GetValue(i));
                    }
                    for (size_t i = 0; i < probe_tuple.GetValueCount(); ++i)
                    {
                        joined_values.push_back(probe_tuple.GetValue(i));
                    }
                }
                *tuple = Tuple(std::move(joined_values));
                return true;
            }

            ++probe_cursor_;
            match_cursor_ = 0;
            current_matches_.clear();
        }

        return false;
    }

    void HashJoin::Close()
    {
        hash_table_.clear();
        probe_rows_ = nullptr;
        build_rows_ = nullptr;
        probe_cursor_ = 0;
        match_cursor_ = 0;
        current_matches_.clear();
    }

} // namespace sql
