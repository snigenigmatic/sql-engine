#pragma once

#include "execution/operator.h"
#include "storage/table.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sql
{

    class HashJoin : public Operator
    {
    public:
        // build_right=true means build hash table on right table and probe with left table.
        HashJoin(Table *left_table, Table *right_table, std::string left_column, std::string right_column, bool build_right = true);

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

        Table *left_table_;
        Table *right_table_;
        std::string left_column_;
        std::string right_column_;
        bool build_right_ = true;

        int left_column_index_ = -1;
        int right_column_index_ = -1;

        std::unordered_map<JoinKey, std::vector<size_t>, JoinKeyHasher> hash_table_;
        const std::vector<Tuple> *probe_rows_ = nullptr;
        const std::vector<Tuple> *build_rows_ = nullptr;
        bool probe_is_left_ = true;

        size_t probe_cursor_ = 0;
        size_t match_cursor_ = 0;
        std::vector<size_t> current_matches_;

        static JoinKey MakeJoinKey(const Value &value);
    };

} // namespace sql
