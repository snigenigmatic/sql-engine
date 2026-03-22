#include "execution/executor.h"
#include <stdexcept>

namespace sql
{

    Value Executor::EvaluateExpr(const Expression *expr, const Tuple *tuple, Table *table) const
    {
        if (!expr)
            throw std::runtime_error("Null expression");

        switch (expr->GetType())
        {
        case ExpressionType::LITERAL:
            return static_cast<const LiteralExpression *>(expr)->value;
        case ExpressionType::COLUMN_REF:
        {
            if (!tuple || !table)
                throw std::runtime_error("Column reference without tuple context");
            const auto *col = static_cast<const ColumnExpression *>(expr);
            int idx = table->GetColumnIndex(col->name);
            if (idx < 0)
                throw std::runtime_error("Unknown column: " + col->name);
            return tuple->GetValue(static_cast<size_t>(idx));
        }
        case ExpressionType::BINARY_OP:
        {
            const auto *bin = static_cast<const BinaryExpression *>(expr);
            Value left = EvaluateExpr(bin->left.get(), tuple, table);
            Value right = EvaluateExpr(bin->right.get(), tuple, table);
            switch (bin->op)
            {
            case TokenType::EQ:
                return Value(left == right);
            case TokenType::NEQ:
                return Value(left != right);
            case TokenType::LT:
                return Value(left < right);
            case TokenType::GT:
                return Value(left > right);
            case TokenType::LEQ:
                return Value(left <= right);
            case TokenType::GEQ:
                return Value(left >= right);
            case TokenType::AND:
                return Value(left.GetAsBool() && right.GetAsBool());
            case TokenType::OR:
                return Value(left.GetAsBool() || right.GetAsBool());
            case TokenType::PLUS:
                if (left.GetType() == DataType::INTEGER && right.GetType() == DataType::INTEGER)
                    return Value(left.GetAsInt() + right.GetAsInt());
                return Value(left.GetAsFloat() + right.GetAsFloat());
            case TokenType::MINUS:
                if (left.GetType() == DataType::INTEGER && right.GetType() == DataType::INTEGER)
                    return Value(left.GetAsInt() - right.GetAsInt());
                return Value(left.GetAsFloat() - right.GetAsFloat());
            case TokenType::STAR:
                if (left.GetType() == DataType::INTEGER && right.GetType() == DataType::INTEGER)
                    return Value(left.GetAsInt() * right.GetAsInt());
                return Value(left.GetAsFloat() * right.GetAsFloat());
            case TokenType::SLASH:
                if (left.GetType() == DataType::INTEGER && right.GetType() == DataType::INTEGER)
                {
                    if (right.GetAsInt() == 0)
                        throw std::runtime_error("Division by zero");
                    return Value(left.GetAsInt() / right.GetAsInt());
                }
                if (right.GetAsFloat() == 0.0)
                    throw std::runtime_error("Division by zero");
                return Value(left.GetAsFloat() / right.GetAsFloat());
            default:
                throw std::runtime_error("Unknown binary operator");
            }
        }
        default:
            throw std::runtime_error("Unknown expression type");
        }
    }

    ExecutionResult Executor::Execute(Statement *stmt)
    {
        if (!stmt)
            return {false, "Null statement", {}, {}};

        switch (stmt->GetType())
        {
        case StatementType::SELECT:
            return ExecuteSelect(static_cast<SelectStatement *>(stmt));
        case StatementType::CREATE_TABLE:
            return ExecuteCreateTable(static_cast<CreateTableStatement *>(stmt));
        case StatementType::INSERT:
            return ExecuteInsert(static_cast<InsertStatement *>(stmt));
        case StatementType::DELETE_STMT:
            return ExecuteDelete(static_cast<DeleteStatement *>(stmt));
        case StatementType::UPDATE_STMT:
            return ExecuteUpdate(static_cast<UpdateStatement *>(stmt));
        case StatementType::CREATE_INDEX:
            return ExecuteCreateIndex(static_cast<CreateIndexStatement *>(stmt));
        case StatementType::DROP_TABLE:
            return ExecuteDropTable(static_cast<DropTableStatement *>(stmt));
        default:
            return {false, "Unsupported statement type", {}, {}};
        }
    }

    std::vector<int> Executor::ResolveProjectionIndices(const PhysicalPlanNode *node, Table *table) const
    {
        std::vector<int> column_indices;
        if (node == nullptr || table == nullptr || node->project_all)
        {
            return column_indices;
        }

        for (const auto &col_name : node->projected_columns)
        {
            int idx = table->GetColumnIndex(col_name);
            if (idx < 0)
                throw std::runtime_error("Unknown column: " + col_name);
            column_indices.push_back(idx);
        }
        return column_indices;
    }

    std::unique_ptr<Operator> Executor::BuildOperatorTree(const PhysicalPlanNode *node, Table *table)
    {
        if (node == nullptr)
        {
            throw std::runtime_error("Null physical plan node");
        }

        switch (node->type)
        {
        case PhysicalPlanType::SEQ_SCAN:
            return std::make_unique<SeqScan>(table);
        case PhysicalPlanType::INDEX_SCAN:
        {
            BTree *index = catalog_->GetIndex(node->table_name, node->index_column);
            if (!index)
                throw std::runtime_error("Expected index not found on " + node->table_name + "." + node->index_column);

            if (node->is_point_lookup)
            {
                if (!node->point_key.has_value())
                    throw std::runtime_error("Point lookup index scan missing key");
                return std::make_unique<IndexScan>(table, index, *node->point_key);
            }
            return std::make_unique<IndexScan>(
                table, index,
                node->low_key, node->low_inclusive,
                node->high_key, node->high_inclusive);
        }
        case PhysicalPlanType::FILTER:
        {
            if (node->children.empty())
                throw std::runtime_error("Filter node missing child");
            auto child = BuildOperatorTree(node->children[0].get(), table);
            return std::make_unique<Filter>(std::move(child), node->predicate, table);
        }
        case PhysicalPlanType::PROJECTION:
        {
            if (node->children.empty())
                throw std::runtime_error("Projection node missing child");
            auto child = BuildOperatorTree(node->children[0].get(), table);
            auto column_indices = ResolveProjectionIndices(node, table);
            return std::make_unique<Projection>(std::move(child), std::move(column_indices), node->project_all);
        }
        default:
            throw std::runtime_error("Unknown physical plan node type");
        }
    }

    std::unique_ptr<Operator> Executor::BuildPlan(SelectStatement *select)
    {
        Table *table = catalog_->GetTable(select->table);
        if (!table)
            throw std::runtime_error("Table not found: " + select->table);

        Optimizer optimizer;
        auto physical_plan = optimizer.BuildPhysicalPlan(select, catalog_);
        return BuildOperatorTree(physical_plan.get(), table);
    }

    ExecutionResult Executor::ExecuteSelect(SelectStatement *select)
    {
        ExecutionResult result;
        try
        {
            auto plan = BuildPlan(select);
            Table *table = catalog_->GetTable(select->table);

            if (select->select_star)
            {
                for (const auto &col : table->GetSchema().GetColumns())
                    result.column_names.push_back(col.name);
            }
            else
            {
                result.column_names = select->columns;
            }

            plan->Open();
            Tuple tuple;
            while (plan->Next(&tuple))
            {
                result.tuples.push_back(tuple);
            }
            plan->Close();
            result.success = true;
        }
        catch (const std::exception &e)
        {
            result.success = false;
            result.message = e.what();
        }
        return result;
    }

    ExecutionResult Executor::ExecuteCreateTable(CreateTableStatement *create)
    {
        ExecutionResult result;
        try
        {
            std::vector<Column> columns;
            for (const auto &cd : create->columns)
            {
                DataType dt;
                switch (cd.type_token)
                {
                case TokenType::INTEGER:
                    dt = DataType::INTEGER;
                    break;
                case TokenType::FLOAT:
                    dt = DataType::FLOAT;
                    break;
                case TokenType::VARCHAR:
                    dt = DataType::VARCHAR;
                    break;
                case TokenType::BOOLEAN:
                    dt = DataType::BOOLEAN;
                    break;
                default:
                    throw std::runtime_error("Unknown column type");
                }
                columns.emplace_back(cd.name, dt, cd.length);
            }

            Schema schema(columns);
            if (!catalog_->CreateTable(create->table, schema))
            {
                result.success = false;
                result.message = "Table '" + create->table + "' already exists.";
                return result;
            }

            result.success = true;
            result.message = "Table '" + create->table + "' created.";
        }
        catch (const std::exception &e)
        {
            result.success = false;
            result.message = e.what();
        }
        return result;
    }

    ExecutionResult Executor::ExecuteInsert(InsertStatement *insert)
    {
        ExecutionResult result;
        try
        {
            Table *table = catalog_->GetTable(insert->table);
            if (!table)
                throw std::runtime_error("Table not found: " + insert->table);

            size_t col_count = table->GetSchema().GetColumnCount();
            size_t rows_inserted = 0;

            for (auto &row : insert->rows)
            {
                if (row.size() != col_count)
                    throw std::runtime_error("Column count mismatch: expected " +
                                             std::to_string(col_count) + ", got " +
                                             std::to_string(row.size()));

                std::vector<Value> values;
                for (auto &expr : row)
                    values.push_back(EvaluateExpr(expr.get()));

                table->Insert(Tuple(std::move(values)));
                rows_inserted++;
            }

            result.success = true;
            result.message = std::to_string(rows_inserted) + " row(s) inserted.";
            catalog_->RebuildIndexesForTable(insert->table);
        }
        catch (const std::exception &e)
        {
            result.success = false;
            result.message = e.what();
        }
        return result;
    }

    ExecutionResult Executor::ExecuteDelete(DeleteStatement *del)
    {
        ExecutionResult result;
        try
        {
            Table *table = catalog_->GetTable(del->table);
            if (!table)
                throw std::runtime_error("Table not found: " + del->table);

            const auto &tuples = table->GetTuples();
            std::vector<size_t> to_delete;

            for (size_t i = 0; i < tuples.size(); ++i)
            {
                if (!del->where)
                {
                    to_delete.push_back(i);
                }
                else
                {
                    Value v = EvaluateExpr(del->where.get(), &tuples[i], table);
                    if (v.GetAsBool())
                        to_delete.push_back(i);
                }
            }

            table->DeleteByIndices(to_delete);
            catalog_->RebuildIndexesForTable(del->table);
            result.success = true;
            result.message = std::to_string(to_delete.size()) + " row(s) deleted.";
        }
        catch (const std::exception &e)
        {
            result.success = false;
            result.message = e.what();
        }
        return result;
    }

    ExecutionResult Executor::ExecuteUpdate(UpdateStatement *update)
    {
        ExecutionResult result;
        try
        {
            Table *table = catalog_->GetTable(update->table);
            if (!table)
                throw std::runtime_error("Table not found: " + update->table);

            std::vector<std::pair<size_t, Tuple>> updates; // (index, new tuple)
            size_t idx = 0;

            for (auto &existing_tuple : *table)
            {
                bool matches = true;
                if (update->where)
                {
                    Value v = EvaluateExpr(update->where.get(), &existing_tuple, table);
                    matches = v.GetAsBool();
                }

                if (matches)
                {
                    // Build new values from existing tuple
                    std::vector<Value> new_values;
                    for (size_t c = 0; c < table->GetSchema().GetColumnCount(); ++c)
                        new_values.push_back(existing_tuple.GetValue(c));

                    // Apply SET assignments
                    for (const auto &assign : update->assignments)
                    {
                        int col_idx = table->GetColumnIndex(assign.first);
                        if (col_idx < 0)
                            throw std::runtime_error("Unknown column: " + assign.first);
                        new_values[static_cast<size_t>(col_idx)] = EvaluateExpr(assign.second.get(), &existing_tuple, table);
                    }

                    updates.push_back({idx, Tuple(std::move(new_values))});
                }
                ++idx;
            }

            // Apply all updates
            for (const auto &update_pair : updates)
                table->UpdateTuple(update_pair.first, update_pair.second);

            catalog_->RebuildIndexesForTable(update->table);
            result.success = true;
            result.message = std::to_string(updates.size()) + " row(s) updated.";
        }
        catch (const std::exception &e)
        {
            result.success = false;
            result.message = e.what();
        }
        return result;
    }

    ExecutionResult Executor::ExecuteCreateIndex(CreateIndexStatement *create)
    {
        ExecutionResult result;
        if (!catalog_->CreateIndex(create->index_name, create->table, create->column))
        {
            result.success = false;
            result.message = "Failed to create index '" + create->index_name +
                             "'. Table or column may not exist, or index name is already taken.";
            return result;
        }
        result.success = true;
        result.message = "Index '" + create->index_name + "' created on " +
                         create->table + "(" + create->column + ").";
        return result;
    }

    ExecutionResult Executor::ExecuteDropTable(DropTableStatement *drop)
    {
        if (!catalog_->DropTable(drop->table))
            return {false, "Table '" + drop->table + "' does not exist.", {}, {}};
        return {true, "Table '" + drop->table + "' dropped.", {}, {}};
    }

} // namespace sql
