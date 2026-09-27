#pragma once

#include "catalog/catalog.h"
#include "parser/ast.h"
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sql
{

    enum class LogicalPlanType
    {
        SEQ_SCAN,
        FILTER,
        AGGREGATE,
        PROJECTION
    };

    struct LogicalPlanNode
    {
        explicit LogicalPlanNode(LogicalPlanType t) : type(t) {}

        LogicalPlanType type;
        std::vector<std::unique_ptr<LogicalPlanNode>> children;

        // SEQ_SCAN
        std::string table_name;

        // FILTER
        const Expression *predicate = nullptr; // non-owning, refers to AST owned by Statement

        // AGGREGATE: SQL text of the GROUP BY expressions
        std::vector<std::string> group_labels;

        // PROJECTION
        bool project_all = false;
        std::vector<std::string> projected_columns;
    };

    enum class PhysicalPlanType
    {
        SEQ_SCAN,
        INDEX_SCAN,
        NESTED_LOOP_JOIN,
        HASH_JOIN,
        INDEX_NESTED_LOOP_JOIN,
        FILTER,
        PROJECTION,
        SORT,
        DISTINCT,
        LIMIT,
        AGGREGATE
    };

    struct PhysicalPlanNode
    {
        explicit PhysicalPlanNode(PhysicalPlanType t) : type(t) {}

        PhysicalPlanType type;
        std::vector<std::unique_ptr<PhysicalPlanNode>> children;

        // Row shape. Scans, and filters pushed below the joins, produce rows
        // of one relation (its index in the Scope); everything else produces
        // rows of the first relation_count relations joined.
        std::string table_name;
        int relation = -1;
        size_t relation_count = 0;

        // INDEX_SCAN
        std::string index_column;
        bool is_point_lookup = false;
        std::optional<Value> point_key;
        std::optional<Value> low_key;
        bool low_inclusive = true;
        std::optional<Value> high_key;
        bool high_inclusive = true;

        // NESTED_LOOP_JOIN / HASH_JOIN / INDEX_NESTED_LOOP_JOIN: children
        // are the left input (the relations joined so far) and the right
        // relation's access path; output rows are left || right
        JoinType join_type = JoinType::INNER;
        std::string right_table_name;
        std::string right_label;                      // "table" or "table AS alias"
        std::vector<const Expression *> join_residual; // ON conditions checked on joined rows
        size_t left_key = 0;                           // HASH / INDEX: key column in left rows
        size_t right_key = 0;                          // ... and in the right relation's rows
        std::string join_key_label;                    // "a.x = b.y"
        std::string join_on_label;                     // the whole ON condition
        bool join_build_right = true;                  // HASH_JOIN
        bool join_outer_is_left = true;                // INDEX: false probes the left table's index

        // FILTER
        const Expression *predicate = nullptr; // non-owning, refers to AST owned by Statement

        // PROJECTION
        bool project_all = false;
        std::vector<std::string> projected_columns; // names, or SQL text of computed items
        // Set when some SELECT item is computed: evaluate these expressions
        // instead of copying columns (non-owning, AST owned by Statement)
        bool compute_projection = false;
        std::vector<const Expression *> projected_exprs;

        // SORT: keys evaluated on the rows below the projection (non-owning,
        // except keys the planner had to create, kept in owned_exprs)
        std::vector<const Expression *> sort_keys;
        std::vector<bool> sort_descending;
        std::vector<std::unique_ptr<Expression>> owned_exprs;

        // LIMIT
        std::optional<int64_t> limit;
        int64_t offset = 0;

        // AGGREGATE: group keys and aggregates, evaluated on the input rows
        // (non-owning, AST owned by Statement). Output rows hold the key
        // values, then the aggregate results, in columns with these names.
        std::vector<const Expression *> group_keys;
        std::vector<const AggregateExpression *> aggregates;
        std::vector<std::string> aggregate_columns;

        // FILTER / SORT / PROJECTION: the rows are an AGGREGATE's output, and
        // the expressions refer to its columns
        bool over_aggregate = false;
    };

    // The tables in a SELECT's FROM clause, in join order, each known by
    // its alias or else its name. Joined rows hold every relation's
    // columns, in this order.
    class Scope
    {
    public:
        struct Relation
        {
            std::string name;  // alias, else table name
            std::string table; // table name in the catalog
            Table *schema;
        };
        struct Resolved
        {
            size_t relation;
            size_t column;
        };

        static Scope ForSelect(const SelectStatement &select, Catalog *catalog);

        const std::vector<Relation> &Relations() const { return relations_; }
        size_t Size() const { return relations_.size(); }
        // Position of a relation's first column in joined rows
        size_t Offset(size_t relation) const;

        // The column a name refers to among the first `count` relations.
        // Throws for an unknown table qualifier, unknown or ambiguous column.
        Resolved Resolve(const std::string &name, size_t count = SIZE_MAX) const;
        bool CanResolve(const std::string &name, size_t count = SIZE_MAX) const;
        // "relation.column" when the name resolves, else the name itself
        std::string Canonical(const std::string &name) const;
        // Bit i set when the expression reads relation i; nullopt if some
        // column does not resolve
        std::optional<uint64_t> RelationsOf(const Expression *expr, size_t count = SIZE_MAX) const;

    private:
        std::vector<Relation> relations_;
    };

    class Optimizer
    {
    public:
        Optimizer() = default;

        std::unique_ptr<LogicalPlanNode> BuildLogicalPlan(const Statement *stmt) const;
        std::unique_ptr<PhysicalPlanNode> BuildPhysicalPlan(const Statement *stmt, Catalog *catalog) const;
        std::string ExplainLogicalPlan(const LogicalPlanNode *root) const;
        std::string ExplainPhysicalPlan(const PhysicalPlanNode *root) const;

    private:
        // ORDER BY items as sort keys: positions and output aliases become the
        // selected expression they name
        void ResolveSortKeys(const SelectStatement &select, const Scope &scope, PhysicalPlanNode *sort) const;

        // Aggregation, HAVING, ORDER BY and the SELECT list of a query with
        // GROUP BY or aggregate functions, over its (filtered) input rows
        std::unique_ptr<PhysicalPlanNode> BuildAggregatePlan(const SelectStatement &select, const Scope &scope,
                                                             std::unique_ptr<PhysicalPlanNode> input) const;

        std::unique_ptr<LogicalPlanNode> BuildSelectLogicalPlan(const SelectStatement *select) const;
        std::unique_ptr<PhysicalPlanNode> BuildSelectPhysicalPlan(const SelectStatement *select, Catalog *catalog) const;
        std::string ExplainNode(const LogicalPlanNode *node, int indent) const;
        std::string ExplainPhysicalNode(const PhysicalPlanNode *node, int indent) const;
    };

} // namespace sql
