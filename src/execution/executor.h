#pragma once

#include "execution/operator.h"
#include "execution/seq_scan.h"
#include "execution/filter.h"
#include "execution/projection.h"
#include "execution/index_scan.h"
#include "parser/ast.h"
#include "optimizer/optimizer.h"
#include "catalog/catalog.h"
#include <memory>
#include <vector>
#include <string>

namespace sql
{

    struct ExecutionResult
    {
        bool success = false;
        std::string message;
        std::vector<Tuple> tuples;
        std::vector<std::string> column_names;
    };

    class Executor
    {
    public:
        explicit Executor(Catalog *catalog) : catalog_(catalog) {}

        ExecutionResult Execute(Statement *stmt);

    private:
        std::unique_ptr<Operator> BuildPlan(SelectStatement *select);
        std::unique_ptr<Operator> BuildOperatorTree(const PhysicalPlanNode *node, Table *table);
        std::vector<int> ResolveProjectionIndices(const PhysicalPlanNode *node, Table *table) const;
        ExecutionResult ExecuteSelect(SelectStatement *select);
        ExecutionResult ExecuteCreateTable(CreateTableStatement *create);
        ExecutionResult ExecuteInsert(InsertStatement *insert);
        ExecutionResult ExecuteDelete(DeleteStatement *del);
        ExecutionResult ExecuteUpdate(UpdateStatement *update);
        ExecutionResult ExecuteCreateIndex(CreateIndexStatement *create);
        ExecutionResult ExecuteDropTable(DropTableStatement *drop);

        // Evaluate an expression (reused for INSERT values, UPDATE SET, etc.)
        Value EvaluateExpr(const Expression *expr, const Tuple *tuple = nullptr, Table *table = nullptr) const;

        Catalog *catalog_;
    };

} // namespace sql
