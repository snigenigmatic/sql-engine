#pragma once

#include "execution/operator.h"
#include "execution/seq_scan.h"
#include "execution/filter.h"
#include "execution/projection.h"
#include "execution/index_scan.h"
#include "execution/nested_loop_join.h"
#include "execution/hash_join.h"
#include "parser/ast.h"
#include "optimizer/optimizer.h"
#include "catalog/catalog.h"
#include "storage/table.h"
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
        std::unique_ptr<Operator> BuildOperatorTree(const PhysicalPlanNode *node, Table *table, Table *join_table = nullptr);
        std::vector<int> ResolveProjectionIndices(const PhysicalPlanNode *node, Table *table, Table *join_table = nullptr) const;
        ExecutionResult ExecuteSelect(SelectStatement *select);
        ExecutionResult ExecuteCreateTable(CreateTableStatement *create);
        ExecutionResult ExecuteInsert(InsertStatement *insert);
        ExecutionResult ExecuteDelete(DeleteStatement *del);
        ExecutionResult ExecuteUpdate(UpdateStatement *update);
        ExecutionResult ExecuteCreateIndex(CreateIndexStatement *create);
        ExecutionResult ExecuteDropTable(DropTableStatement *drop);

        // Evaluate an expression (reused for INSERT values, UPDATE SET, etc.)
        Value EvaluateExpr(const Expression *expr, const Tuple *tuple = nullptr, Table *table = nullptr) const;
        int ResolveColumnIndexForSelect(const std::string &name, Table *base_table, Table *join_table, bool *from_join_table = nullptr) const;
        void EnsureJoinContextTable(Table *left, Table *right);
        std::pair<std::string, std::string> ResolveJoinColumns(const PhysicalPlanNode *node, Table *left, Table *right) const;

        Catalog *catalog_;
        std::unique_ptr<Table> join_context_table_;
    };

} // namespace sql
