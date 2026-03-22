#include "optimizer/optimizer.h"
#include <stdexcept>
#include <sstream>

namespace sql
{
    namespace
    {
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

            auto join = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::NESTED_LOOP_JOIN);
            join->table_name = select->table;
            join->right_table_name = *select->join_table;
            join->join_left_column = *select->join_left_column;
            join->join_right_column = *select->join_right_column;
            // Rule-based choice: iterate smaller table in outer loop.
            join->join_right_as_outer = right_table->GetTupleCount() < table->GetTupleCount();
            current = std::move(join);
        }
        else
        {
            std::string column_name;
            TokenType op = TokenType::ILLEGAL;
            Value literal_value;
            if (IsIndexableComparison(select->where.get(), &column_name, &op, &literal_value))
            {
                BTree *index = catalog->GetIndex(select->table, column_name);
                if (index != nullptr)
                {
                    int col_idx = table->GetColumnIndex(column_name);
                    if (col_idx < 0)
                    {
                        throw std::runtime_error("Indexed column not found in table schema: " + column_name);
                    }

                    const Column &column = table->GetSchema().GetColumn(static_cast<size_t>(col_idx));
                    if (column.type != literal_value.GetType())
                    {
                        index = nullptr;
                    }
                }

                if (index != nullptr)
                {
                    (void)index; // Existence check is enough for plan construction

                    auto index_scan = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::INDEX_SCAN);
                    index_scan->table_name = select->table;
                    index_scan->index_column = column_name;

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
                    current = std::move(index_scan);
                }
            }

            if (!current)
            {
                auto seq_scan = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::SEQ_SCAN);
                seq_scan->table_name = select->table;
                current = std::move(seq_scan);
            }
        }

        if (select->where != nullptr)
        {
            auto filter = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::FILTER);
            filter->predicate = select->where.get();
            filter->children.push_back(std::move(current));
            current = std::move(filter);
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
