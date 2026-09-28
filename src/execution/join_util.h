#pragma once

#include "common/schema.h"
#include "common/tuple.h"
#include "parser/ast.h"
#include "storage/table.h"
#include <vector>

namespace sql
{

    // What every join operator shares: output rows are left || right, the
    // ON conditions not used as the join key (the residual) must also hold,
    // and a LEFT join pads an unmatched left row with NULLs.
    struct JoinOutput
    {
        JoinType type = JoinType::INNER;
        std::vector<const Expression *> residual; // not owned
        Table *context = nullptr;                 // shape of the joined rows
        const Schema *right_schema = nullptr;     // for NULL padding

        static Tuple Concat(const Tuple &left, const Tuple &right);
        // True when every residual condition is TRUE for the joined row
        bool Holds(const Tuple &joined) const;
        Tuple PadRight(const Tuple &left) const;
    };

} // namespace sql
