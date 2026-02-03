#pragma once

#include "execution/operator.h"
#include "execution/seq_scan.h"
#include "execution/filter.h"
#include "execution/projection.h"
#include "parser/ast.h"
#include "catalog/catalog.h"
#include <memory>
#include <vector>
#include <string>

namespace sql
{

    // Result of executing a query
    struct ExecutionResult
    {
        bool success = false;
        std::string error_message;
        std::vector<Tuple> tuples;
        std::vector<std::string> column_names; // For display
    };

    class Executor
    {
    public:
        explicit Executor(Catalog *catalog) : catalog_(catalog) {}

        // Execute a statement and return results
        ExecutionResult Execute(Statement *stmt);

    private:
        // Build an execution plan for a SELECT statement
        std::unique_ptr<Operator> BuildPlan(SelectStatement *select);

        // Execute a SELECT statement
        ExecutionResult ExecuteSelect(SelectStatement *select);

        Catalog *catalog_;
    };

} // namespace sql
