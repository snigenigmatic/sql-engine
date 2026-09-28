#pragma once

#include "execution/join_util.h"
#include "execution/operator.h"
#include <memory>
#include "storage/table.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sql
{

    // Equi-join through a hash table on the key columns. build_right hashes
    // the right input and streams the left rows past it, which also serves
    // LEFT joins; otherwise (INNER only) the left rows are hashed and the
    // right input streamed. NULL keys never match.
    class HashJoin : public Operator
    {
    public:
        HashJoin(std::unique_ptr<Operator> left, Table *right, size_t left_key, size_t right_key, JoinOutput output,
                 bool build_right = true);

        void Open() override;
        bool Next(Tuple *tuple) override;
        void Close() override;

    private:
        struct JoinKey
        {
            DataType type = DataType::INTEGER;
            bool is_null = true;
            int32_t int_value = 0;
            double float_value = 0.0;
            bool bool_value = false;
            std::string string_value;

            bool operator==(const JoinKey &other) const
            {
                if (type != other.type || is_null != other.is_null)
                {
                    return false;
                }
                if (is_null)
                {
                    return true;
                }
                switch (type)
                {
                case DataType::INTEGER:
                    return int_value == other.int_value;
                case DataType::FLOAT:
                    return float_value == other.float_value;
                case DataType::BOOLEAN:
                    return bool_value == other.bool_value;
                case DataType::VARCHAR:
                    return string_value == other.string_value;
                default:
                    return false;
                }
            }
        };

        struct JoinKeyHasher
        {
            size_t operator()(const JoinKey &key) const;
        };

        std::unique_ptr<Operator> left_;
        Table *right_;
        size_t left_key_;
        size_t right_key_;
        JoinOutput output_;
        bool build_right_ = true;

        // Build rows; hash table values index into build_rows_
        std::unordered_map<JoinKey, std::vector<size_t>, JoinKeyHasher> hash_table_;
        std::vector<Tuple> build_rows_;

        // Probe side: the left operator, or the right table's rows
        std::vector<Tuple> probe_rows_;
        size_t probe_cursor_ = 0;
        bool NextProbe(Tuple *row);

        Tuple probe_tuple_;
        bool probe_valid_ = false;
        bool matched_ = false;
        size_t match_cursor_ = 0;
        const std::vector<size_t> *current_matches_ = nullptr;

        static JoinKey MakeJoinKey(const Value &value);
    };

} // namespace sql
