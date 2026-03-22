#pragma once

#include "catalog/catalog.h"
#include "parser/ast.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sql
{

    enum class LogicalPlanType
    {
        SEQ_SCAN,
        FILTER,
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

        // PROJECTION
        bool project_all = false;
        std::vector<std::string> projected_columns;
    };

    enum class PhysicalPlanType
    {
        SEQ_SCAN,
        INDEX_SCAN,
        NESTED_LOOP_JOIN,
        FILTER,
        PROJECTION
    };

    struct PhysicalPlanNode
    {
        explicit PhysicalPlanNode(PhysicalPlanType t) : type(t) {}

        PhysicalPlanType type;
        std::vector<std::unique_ptr<PhysicalPlanNode>> children;

        // Common context
        std::string table_name;
        std::string right_table_name;

        // INDEX_SCAN
        std::string index_column;
        bool is_point_lookup = false;
        std::optional<Value> point_key;
        std::optional<Value> low_key;
        bool low_inclusive = true;
        std::optional<Value> high_key;
        bool high_inclusive = true;

        // NESTED_LOOP_JOIN
        std::string join_left_column;
        std::string join_right_column;
        bool join_right_as_outer = false;

        // FILTER
        const Expression *predicate = nullptr; // non-owning, refers to AST owned by Statement

        // PROJECTION
        bool project_all = false;
        std::vector<std::string> projected_columns;
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
        std::unique_ptr<LogicalPlanNode> BuildSelectLogicalPlan(const SelectStatement *select) const;
        std::unique_ptr<PhysicalPlanNode> BuildSelectPhysicalPlan(const SelectStatement *select, Catalog *catalog) const;
        std::string ExplainNode(const LogicalPlanNode *node, int indent) const;
        std::string ExplainPhysicalNode(const PhysicalPlanNode *node, int indent) const;
    };

} // namespace sql
