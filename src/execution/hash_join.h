#pragma once

#include "execution/operator.h"
#include "storage/table.h"
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
        Table *left_table_;
        Table *right_table_;
        std::string left_column_;
        std::string right_column_;
        bool build_right_ = true;

        int left_column_index_ = -1;
        int right_column_index_ = -1;

        std::unordered_map<std::string, std::vector<size_t>> hash_table_;
        const std::vector<Tuple> *probe_rows_ = nullptr;
        const std::vector<Tuple> *build_rows_ = nullptr;
        bool probe_is_left_ = true;

        size_t probe_cursor_ = 0;
        size_t match_cursor_ = 0;
        std::vector<size_t> current_matches_;
    };

} // namespace sql
