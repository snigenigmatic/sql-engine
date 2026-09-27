#pragma once

#include "execution/operator.h"
#include "execution/seq_scan.h"
#include "execution/filter.h"
#include "execution/projection.h"
#include "execution/expression_projection.h"
#include "execution/sort.h"
#include "execution/distinct.h"
#include "execution/aggregate.h"
#include "execution/limit.h"
#include "execution/index_scan.h"
#include "execution/nested_loop_join.h"
#include "execution/hash_join.h"
#include "execution/index_nested_loop_join.h"
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
        std::unique_ptr<Operator> BuildOperatorTree(const PhysicalPlanNode *node);
        // The right input of a join: the table itself, or its filtered rows
        Table *JoinRightInput(const PhysicalPlanNode *access_path, const Scope::Relation &relation);
        ExecutionResult ExecuteSelect(SelectStatement *select);
        ExecutionResult ExecuteCreateTable(CreateTableStatement *create);
        ExecutionResult ExecuteInsert(InsertStatement *insert);
        ExecutionResult ExecuteDelete(DeleteStatement *del);
        ExecutionResult ExecuteUpdate(UpdateStatement *update);
        ExecutionResult ExecuteCreateIndex(CreateIndexStatement *create);
        ExecutionResult ExecuteDropTable(DropTableStatement *drop);
        ExecutionResult ExecuteExplain(ExplainStatement *explain);

        // Evaluate an expression (reused for INSERT values, UPDATE SET, etc.)
        Value EvaluateExpr(const Expression *expr, const Tuple *tuple = nullptr, Table *table = nullptr) const;
        Value ResolveColumnValue(const ColumnExpression &col, const Tuple *tuple, Table *table) const;
        // The value as the column stores it (NULL typed, numbers converted),
        // after checking NOT NULL, the column's type and VARCHAR length.
        // Throws std::runtime_error naming table.column on a violation.
        static Value CoerceToColumn(const Value &value, const Column &column, const std::string &table);
        // Rows produced by a plan node are shaped like this table
        Table *RowContext(const PhysicalPlanNode *node);
        // Rows of the first `count` relations joined: the table itself for
        // one, else a schema of their columns named "relation.column"
        Table *JoinedContext(size_t count);
        // Rows of one relation, for resolving names: the table itself, or
        // a schema-only table under the relation's alias
        Table *RelationContext(size_t relation);
        Table *MaterializeOperatorToTable(std::unique_ptr<Operator> op, Table *source_table);

        Catalog *catalog_;
        Scope scope_; // FROM clause of the SELECT being run
        std::vector<std::unique_ptr<Table>> joined_contexts_;   // by relation count
        std::vector<std::unique_ptr<Table>> relation_contexts_; // aliased relations
        // Shape of an AGGREGATE's output rows (group keys, then aggregates);
        // only the column names are used, to resolve references
        std::unique_ptr<Table> aggregate_context_table_;
        std::unique_ptr<PhysicalPlanNode> physical_plan_;
        std::vector<std::unique_ptr<Table>> materialized_tables_;
    };

} // namespace sql
