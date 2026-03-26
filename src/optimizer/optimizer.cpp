#include "optimizer/optimizer.h"
#include <stdexcept>
#include <sstream>

namespace sql
{
    namespace
    {
        enum class PredicateTableSide
        {
            NONE,
            LEFT,
            RIGHT,
            BOTH
        };

        std::string StripQualifier(const std::string &name)
        {
            size_t dot = name.find('.');
            if (dot == std::string::npos)
            {
                return name;
            }
            return name.substr(dot + 1);
        }

        PredicateTableSide ResolveColumnSide(const std::string &column_name,
                                             const std::string &left_table_name,
                                             const std::string &right_table_name,
                                             Table *left_table,
                                             Table *right_table)
        {
            const size_t dot = column_name.find('.');
            if (dot != std::string::npos)
            {
                const std::string qualifier = column_name.substr(0, dot);
                const std::string unqualified = column_name.substr(dot + 1);
                if (qualifier == left_table_name && left_table->GetColumnIndex(unqualified) >= 0)
                {
                    return PredicateTableSide::LEFT;
                }
                if (qualifier == right_table_name && right_table->GetColumnIndex(unqualified) >= 0)
                {
                    return PredicateTableSide::RIGHT;
                }
                return PredicateTableSide::NONE;
            }

            const bool in_left = left_table->GetColumnIndex(column_name) >= 0;
            const bool in_right = right_table->GetColumnIndex(column_name) >= 0;
            if (in_left && in_right)
            {
                return PredicateTableSide::BOTH;
            }
            if (in_left)
            {
                return PredicateTableSide::LEFT;
            }
            if (in_right)
            {
                return PredicateTableSide::RIGHT;
            }
            return PredicateTableSide::NONE;
        }

        PredicateTableSide ResolvePredicateSingleTable(const Expression *expr,
                                                       const std::string &left_table_name,
                                                       const std::string &right_table_name,
                                                       Table *left_table,
                                                       Table *right_table)
        {
            if (expr == nullptr)
            {
                return PredicateTableSide::NONE;
            }

            switch (expr->GetType())
            {
            case ExpressionType::LITERAL:
                return PredicateTableSide::NONE;
            case ExpressionType::COLUMN_REF:
            {
                const auto *col = static_cast<const ColumnExpression *>(expr);
                return ResolveColumnSide(col->name, left_table_name, right_table_name, left_table, right_table);
            }
            case ExpressionType::BINARY_OP:
            {
                const auto *bin = static_cast<const BinaryExpression *>(expr);
                const PredicateTableSide left = ResolvePredicateSingleTable(
                    bin->left.get(), left_table_name, right_table_name, left_table, right_table);
                const PredicateTableSide right = ResolvePredicateSingleTable(
                    bin->right.get(), left_table_name, right_table_name, left_table, right_table);

                if (left == PredicateTableSide::BOTH || right == PredicateTableSide::BOTH)
                {
                    return PredicateTableSide::BOTH;
                }
                if (left == PredicateTableSide::NONE)
                {
                    return right;
                }
                if (right == PredicateTableSide::NONE)
                {
                    return left;
                }
                return left == right ? left : PredicateTableSide::BOTH;
            }
            default:
                return PredicateTableSide::BOTH;
            }
        }

        bool IsIndexableComparison(const Expression *expr,
                                   std::string *column_name,
                                   TokenType *op,
                                   Value *literal_value)
        {
            if (expr == nullptr || expr->GetType() != ExpressionType::BINARY_OP)
            {
                return false;
            }

            const auto *bin = static_cast<const BinaryExpression *>(expr);
            if (bin->left->GetType() != ExpressionType::COLUMN_REF ||
                bin->right->GetType() != ExpressionType::LITERAL)
            {
                return false;
            }

            switch (bin->op)
            {
            case TokenType::EQ:
            case TokenType::GT:
            case TokenType::GEQ:
            case TokenType::LT:
            case TokenType::LEQ:
                break;
            default:
                return false;
            }

            const auto *col_expr = static_cast<const ColumnExpression *>(bin->left.get());
            const auto *lit_expr = static_cast<const LiteralExpression *>(bin->right.get());
            *column_name = col_expr->name;
            *op = bin->op;
            *literal_value = lit_expr->value;
            return true;
        }
    } // namespace

    std::unique_ptr<LogicalPlanNode> Optimizer::BuildLogicalPlan(const Statement *stmt) const
    {
        if (stmt == nullptr)
        {
            throw std::runtime_error("Cannot build logical plan for null statement");
        }

        switch (stmt->GetType())
        {
        case StatementType::SELECT:
            return BuildSelectLogicalPlan(static_cast<const SelectStatement *>(stmt));
        default:
            throw std::runtime_error("Logical planner currently supports SELECT statements only");
        }
    }

    std::unique_ptr<LogicalPlanNode> Optimizer::BuildSelectLogicalPlan(const SelectStatement *select) const
    {
        auto scan = std::make_unique<LogicalPlanNode>(LogicalPlanType::SEQ_SCAN);
        scan->table_name = select->table;

        std::unique_ptr<LogicalPlanNode> current = std::move(scan);

        if (select->where != nullptr)
        {
            auto filter = std::make_unique<LogicalPlanNode>(LogicalPlanType::FILTER);
            filter->predicate = select->where.get();
            filter->children.push_back(std::move(current));
            current = std::move(filter);
        }

        auto projection = std::make_unique<LogicalPlanNode>(LogicalPlanType::PROJECTION);
        projection->project_all = select->select_star;
        projection->projected_columns = select->columns;
        projection->children.push_back(std::move(current));
        return projection;
    }

    std::string Optimizer::ExplainNode(const LogicalPlanNode *node, int indent) const
    {
        if (node == nullptr)
        {
            return std::string(static_cast<size_t>(indent * 2), ' ') + "null";
        }

        const std::string pad(static_cast<size_t>(indent * 2), ' ');
        std::ostringstream out;

        switch (node->type)
        {
        case LogicalPlanType::SEQ_SCAN:
            out << pad << "SeqScan(table=" << node->table_name << ")";
            break;
        case LogicalPlanType::FILTER:
            out << pad << "Filter";
            if (node->predicate != nullptr)
            {
                out << "\n" << pad << "  predicate:\n" << DumpExpression(node->predicate, indent + 2);
            }
            break;
        case LogicalPlanType::PROJECTION:
            out << pad << "Projection(columns=";
            if (node->project_all)
            {
                out << "*";
            }
            else
            {
                out << "[";
                for (size_t i = 0; i < node->projected_columns.size(); ++i)
                {
                    if (i > 0)
                    {
                        out << ", ";
                    }
                    out << node->projected_columns[i];
                }
                out << "]";
            }
            out << ")";
            break;
        }

        for (const auto &child : node->children)
        {
            out << "\n" << ExplainNode(child.get(), indent + 1);
        }
        return out.str();
    }

    std::string Optimizer::ExplainLogicalPlan(const LogicalPlanNode *root) const
    {
        return ExplainNode(root, 0);
    }

    std::unique_ptr<PhysicalPlanNode> Optimizer::BuildPhysicalPlan(const Statement *stmt, Catalog *catalog) const
    {
        if (stmt == nullptr)
        {
            throw std::runtime_error("Cannot build physical plan for null statement");
        }
        if (catalog == nullptr)
        {
            throw std::runtime_error("Cannot build physical plan without catalog");
        }

        switch (stmt->GetType())
        {
        case StatementType::SELECT:
            return BuildSelectPhysicalPlan(static_cast<const SelectStatement *>(stmt), catalog);
        default:
            throw std::runtime_error("Physical planner currently supports SELECT statements only");
        }
    }

    std::unique_ptr<PhysicalPlanNode> Optimizer::BuildSelectPhysicalPlan(const SelectStatement *select, Catalog *catalog) const
    {
        Table *table = catalog->GetTable(select->table);
        if (table == nullptr)
        {
            throw std::runtime_error("Table not found: " + select->table);
        }

        auto build_base_access_path = [&](const std::string &table_name, Table *target_table, const Expression *predicate) -> std::unique_ptr<PhysicalPlanNode>
        {
            std::unique_ptr<PhysicalPlanNode> access_path;
            std::string column_name;
            TokenType op = TokenType::ILLEGAL;
            Value literal_value;
            if (IsIndexableComparison(predicate, &column_name, &op, &literal_value))
            {
                const std::string unqualified = StripQualifier(column_name);
                BTree *index = catalog->GetIndex(table_name, unqualified);
                if (index != nullptr)
                {
                    int col_idx = target_table->GetColumnIndex(unqualified);
                    if (col_idx < 0)
                    {
                        throw std::runtime_error("Indexed column not found in table schema: " + unqualified);
                    }

                    const Column &column = target_table->GetSchema().GetColumn(static_cast<size_t>(col_idx));
                    if (column.type != literal_value.GetType())
                    {
                        index = nullptr;
                    }
                }

                if (index != nullptr)
                {
                    auto index_scan = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::INDEX_SCAN);
                    index_scan->table_name = table_name;
                    index_scan->index_column = unqualified;

                    switch (op)
                    {
                    case TokenType::EQ:
                        index_scan->is_point_lookup = true;
                        index_scan->point_key = literal_value;
                        break;
                    case TokenType::GT:
                        index_scan->low_key = literal_value;
                        index_scan->low_inclusive = false;
                        break;
                    case TokenType::GEQ:
                        index_scan->low_key = literal_value;
                        index_scan->low_inclusive = true;
                        break;
                    case TokenType::LT:
                        index_scan->high_key = literal_value;
                        index_scan->high_inclusive = false;
                        break;
                    case TokenType::LEQ:
                        index_scan->high_key = literal_value;
                        index_scan->high_inclusive = true;
                        break;
                    default:
                        break;
                    }
                    access_path = std::move(index_scan);
                }
            }

            if (!access_path)
            {
                auto seq_scan = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::SEQ_SCAN);
                seq_scan->table_name = table_name;
                access_path = std::move(seq_scan);
            }
            return access_path;
        };

        std::unique_ptr<PhysicalPlanNode> current;
        if (select->join_table.has_value())
        {
            Table *right_table = catalog->GetTable(*select->join_table);
            if (right_table == nullptr)
            {
                throw std::runtime_error("Join table not found: " + *select->join_table);
            }
            if (!select->join_left_column.has_value() || !select->join_right_column.has_value())
            {
                throw std::runtime_error("JOIN requires ON left_col = right_col");
            }

            std::unique_ptr<PhysicalPlanNode> left_input = build_base_access_path(select->table, table, nullptr);
            std::unique_ptr<PhysicalPlanNode> right_input = build_base_access_path(*select->join_table, right_table, nullptr);
            const Expression *remaining_predicate = select->where.get();
            if (remaining_predicate != nullptr)
            {
                const PredicateTableSide side = ResolvePredicateSingleTable(
                    remaining_predicate, select->table, *select->join_table, table, right_table);
                if (side == PredicateTableSide::LEFT || side == PredicateTableSide::RIGHT)
                {
                    const std::string target_table_name = (side == PredicateTableSide::LEFT) ? select->table : *select->join_table;
                    Table *target_table = (side == PredicateTableSide::LEFT) ? table : right_table;
                    std::unique_ptr<PhysicalPlanNode> side_input = build_base_access_path(target_table_name, target_table, remaining_predicate);
                    auto side_filter = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::FILTER);
                    side_filter->table_name = target_table_name;
                    side_filter->predicate = remaining_predicate;
                    side_filter->children.push_back(std::move(side_input));
                    side_input = std::move(side_filter);

                    if (side == PredicateTableSide::LEFT)
                    {
                        left_input = std::move(side_input);
                    }
                    else
                    {
                        right_input = std::move(side_input);
                    }
                    remaining_predicate = nullptr;
                }
            }

            const size_t left_count = table->GetTupleCount();
            const size_t right_count = right_table->GetTupleCount();

            // Check if either join column has an index → prefer INDEX_NESTED_LOOP_JOIN.
            const std::string left_col_unq = StripQualifier(*select->join_left_column);
            const std::string right_col_unq = StripQualifier(*select->join_right_column);
            BTree *left_index = catalog->GetIndex(select->table, left_col_unq);
            BTree *right_index = catalog->GetIndex(*select->join_table, right_col_unq);

            std::unique_ptr<PhysicalPlanNode> join;
            if (right_index != nullptr)
            {
                // Right side has index → right is inner, left is outer.
                join = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::INDEX_NESTED_LOOP_JOIN);
                join->join_right_as_outer = false; // left is outer
            }
            else if (left_index != nullptr)
            {
                // Left side has index → left is inner, right is outer.
                join = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::INDEX_NESTED_LOOP_JOIN);
                join->join_right_as_outer = true; // right is outer
            }
            else
            {
                // Rule-based choice between HASH_JOIN and NESTED_LOOP_JOIN.
                const size_t total_rows = left_count + right_count;
                const bool use_hash_join = total_rows >= 16;
                join = std::make_unique<PhysicalPlanNode>(
                    use_hash_join ? PhysicalPlanType::HASH_JOIN : PhysicalPlanType::NESTED_LOOP_JOIN);
                // Iterate smaller table in outer loop for nested loop.
                join->join_right_as_outer = right_count < left_count;
                // Build hash table on smaller side for hash join.
                join->join_build_right = right_count <= left_count;
            }

            join->table_name = select->table;
            join->right_table_name = *select->join_table;
            join->join_left_column = *select->join_left_column;
            join->join_right_column = *select->join_right_column;
            join->children.push_back(std::move(left_input));
            join->children.push_back(std::move(right_input));
            current = std::move(join);

            if (remaining_predicate != nullptr)
            {
                auto filter = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::FILTER);
                filter->table_name = "__join_context__";
                filter->predicate = remaining_predicate;
                filter->children.push_back(std::move(current));
                current = std::move(filter);
            }
        }
        else
        {
            current = build_base_access_path(select->table, table, select->where.get());
            if (select->where != nullptr)
            {
                auto filter = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::FILTER);
                filter->table_name = select->table;
                filter->predicate = select->where.get();
                filter->children.push_back(std::move(current));
                current = std::move(filter);
            }
        }

        auto projection = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::PROJECTION);
        projection->project_all = select->select_star;
        projection->projected_columns = select->columns;
        projection->children.push_back(std::move(current));
        return projection;
    }

    std::string Optimizer::ExplainPhysicalNode(const PhysicalPlanNode *node, int indent) const
    {
        if (node == nullptr)
        {
            return std::string(static_cast<size_t>(indent * 2), ' ') + "null";
        }

        const std::string pad(static_cast<size_t>(indent * 2), ' ');
        std::ostringstream out;

        switch (node->type)
        {
        case PhysicalPlanType::SEQ_SCAN:
            out << pad << "SeqScan(table=" << node->table_name << ")";
            break;
        case PhysicalPlanType::INDEX_SCAN:
            out << pad << "IndexScan(table=" << node->table_name
                << ", column=" << node->index_column;
            if (node->is_point_lookup && node->point_key.has_value())
            {
                out << ", point=" << node->point_key->ToString();
            }
            else
            {
                out << ", low=";
                out << (node->low_key.has_value() ? node->low_key->ToString() : "null");
                out << (node->low_inclusive ? " (inclusive)" : " (exclusive)");
                out << ", high=";
                out << (node->high_key.has_value() ? node->high_key->ToString() : "null");
                out << (node->high_inclusive ? " (inclusive)" : " (exclusive)");
            }
            out << ")";
            break;
        case PhysicalPlanType::NESTED_LOOP_JOIN:
            out << pad << "NestedLoopJoin(left=" << node->table_name
                << ", right=" << node->right_table_name
                << ", on=" << node->join_left_column
                << " = " << node->join_right_column
                << ", outer=" << (node->join_right_as_outer ? "right" : "left")
                << ")";
            break;
        case PhysicalPlanType::HASH_JOIN:
            out << pad << "HashJoin(left=" << node->table_name
                << ", right=" << node->right_table_name
                << ", on=" << node->join_left_column
                << " = " << node->join_right_column
                << ", build=" << (node->join_build_right ? "right" : "left")
                << ")";
            break;
        case PhysicalPlanType::INDEX_NESTED_LOOP_JOIN:
        {
            const std::string &outer = node->join_right_as_outer ? node->right_table_name : node->table_name;
            const std::string &inner = node->join_right_as_outer ? node->table_name : node->right_table_name;
            const std::string &inner_col = node->join_right_as_outer ? node->join_left_column : node->join_right_column;
            out << pad << "IndexNestedLoopJoin(outer=" << outer
                << ", inner=" << inner
                << "(index on " << inner_col << ")"
                << ", on=" << node->join_left_column
                << " = " << node->join_right_column
                << ")";
            break;
        }
        case PhysicalPlanType::FILTER:
            out << pad << "Filter";
            if (node->predicate != nullptr)
            {
                out << "\n" << pad << "  predicate:\n" << DumpExpression(node->predicate, indent + 2);
            }
            break;
        case PhysicalPlanType::PROJECTION:
            out << pad << "Projection(columns=";
            if (node->project_all)
            {
                out << "*";
            }
            else
            {
                out << "[";
                for (size_t i = 0; i < node->projected_columns.size(); ++i)
                {
                    if (i > 0)
                    {
                        out << ", ";
                    }
                    out << node->projected_columns[i];
                }
                out << "]";
            }
            out << ")";
            break;
        }

        for (const auto &child : node->children)
        {
            out << "\n" << ExplainPhysicalNode(child.get(), indent + 1);
        }
        return out.str();
    }

    std::string Optimizer::ExplainPhysicalPlan(const PhysicalPlanNode *root) const
    {
        return ExplainPhysicalNode(root, 0);
    }

} // namespace sql
