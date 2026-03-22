#include "execution/executor.h"
#include <stdexcept>
#include <utility>

namespace sql
{
    namespace
    {
        std::string StripQualifier(const std::string &name)
        {
            size_t dot = name.find('.');
            if (dot == std::string::npos)
            {
                return name;
            }
            return name.substr(dot + 1);
        }
    } // namespace

    int Executor::ResolveColumnIndexForSelect(const std::string &name, Table *base_table, Table *join_table, bool *from_join_table) const
    {
        if (base_table == nullptr)
        {
            throw std::runtime_error("Base table is null while resolving projection");
        }

        const size_t dot = name.find('.');
        if (dot != std::string::npos)
        {
            const std::string qualifier = name.substr(0, dot);
            const std::string column = name.substr(dot + 1);
            if (qualifier == base_table->GetName())
            {
                int idx = base_table->GetColumnIndex(column);
                if (idx < 0)
                    throw std::runtime_error("Unknown column: " + name);
                if (from_join_table)
                    *from_join_table = false;
                return idx;
            }
            if (join_table && qualifier == join_table->GetName())
            {
                int idx = join_table->GetColumnIndex(column);
                if (idx < 0)
                    throw std::runtime_error("Unknown column: " + name);
                if (from_join_table)
                    *from_join_table = true;
                return idx;
            }
            throw std::runtime_error("Unknown table qualifier in column: " + name);
        }

        int base_idx = base_table->GetColumnIndex(name);
        int join_idx = -1;
        if (join_table)
        {
            join_idx = join_table->GetColumnIndex(name);
        }

        if (base_idx >= 0 && join_idx >= 0)
        {
            throw std::runtime_error("Ambiguous column reference: " + name);
        }
        if (base_idx >= 0)
        {
            if (from_join_table)
                *from_join_table = false;
            return base_idx;
        }

        if (join_idx >= 0)
        {
            if (from_join_table)
                *from_join_table = true;
            return join_idx;
        }

        throw std::runtime_error("Unknown column: " + name);
    }

    void Executor::EnsureJoinContextTable(Table *left, Table *right)
    {
        if (left == nullptr || right == nullptr)
        {
            throw std::runtime_error("Cannot create join context schema without both tables");
        }
        std::vector<Column> join_columns;
        join_columns.reserve(left->GetSchema().GetColumnCount() + right->GetSchema().GetColumnCount());
        for (const auto &col : left->GetSchema().GetColumns())
        {
            join_columns.emplace_back(left->GetName() + "." + col.name, col.type, col.length);
        }
        for (const auto &col : right->GetSchema().GetColumns())
        {
            join_columns.emplace_back(right->GetName() + "." + col.name, col.type, col.length);
        }
        join_context_table_ = std::make_unique<Table>("__join_context__", Schema(std::move(join_columns)));
    }

    std::pair<std::string, std::string> Executor::ResolveJoinColumns(const PhysicalPlanNode *node, Table *left, Table *right) const
    {
        std::string left_col = StripQualifier(node->join_left_column);
        std::string right_col = StripQualifier(node->join_right_column);
        if (left->GetColumnIndex(left_col) < 0)
        {
            std::swap(left_col, right_col);
        }
        if (left->GetColumnIndex(left_col) < 0 || right->GetColumnIndex(right_col) < 0)
        {
            throw std::runtime_error("Invalid JOIN columns in ON clause");
        }
        return {left_col, right_col};
    }

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
            {
                const std::string stripped = StripQualifier(col->name);
                int matched_idx = -1;
                const auto &columns = table->GetSchema().GetColumns();
                for (size_t i = 0; i < columns.size(); ++i)
                {
                    const std::string schema_col = columns[i].name;
                    const std::string schema_stripped = StripQualifier(schema_col);
                    if (schema_stripped != stripped)
                    {
                        continue;
                    }
                    if (matched_idx >= 0)
                    {
                        throw std::runtime_error("Ambiguous column: " + col->name);
                    }
                    matched_idx = static_cast<int>(i);
                }
                idx = matched_idx;
            }
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

    std::vector<int> Executor::ResolveProjectionIndices(const PhysicalPlanNode *node, Table *table, Table *join_table) const
    {
        std::vector<int> column_indices;
        if (node == nullptr || table == nullptr || node->project_all)
        {
            return column_indices;
        }

        for (const auto &col_name : node->projected_columns)
        {
            bool from_join = false;
            int idx = ResolveColumnIndexForSelect(col_name, table, join_table, &from_join);
            if (from_join)
            {
                idx += static_cast<int>(table->GetSchema().GetColumnCount());
            }
            column_indices.push_back(idx);
        }
        return column_indices;
    }

    std::unique_ptr<Operator> Executor::BuildOperatorTree(const PhysicalPlanNode *node, Table *table, Table *join_table)
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
        case PhysicalPlanType::NESTED_LOOP_JOIN:
        {
            Table *left = catalog_->GetTable(node->table_name);
            Table *right = catalog_->GetTable(node->right_table_name);
            if (left == nullptr || right == nullptr)
            {
                throw std::runtime_error("JOIN table not found while building operator tree");
            }
            const auto [left_col, right_col] = ResolveJoinColumns(node, left, right);
            EnsureJoinContextTable(left, right);
            return std::make_unique<NestedLoopJoin>(left, right, left_col, right_col, node->join_right_as_outer);
        }
        case PhysicalPlanType::HASH_JOIN:
        {
            Table *left = catalog_->GetTable(node->table_name);
            Table *right = catalog_->GetTable(node->right_table_name);
            if (left == nullptr || right == nullptr)
            {
                throw std::runtime_error("JOIN table not found while building operator tree");
            }
            const auto [left_col, right_col] = ResolveJoinColumns(node, left, right);
            EnsureJoinContextTable(left, right);
            return std::make_unique<HashJoin>(left, right, left_col, right_col, node->join_build_right);
        }
        case PhysicalPlanType::FILTER:
        {
            if (node->children.empty())
                throw std::runtime_error("Filter node missing child");
            auto child = BuildOperatorTree(node->children[0].get(), table, join_table);
            Table *filter_table = (join_context_table_ ? join_context_table_.get() : table);
            return std::make_unique<Filter>(std::move(child), node->predicate, filter_table);
        }
        case PhysicalPlanType::PROJECTION:
        {
            if (node->children.empty())
                throw std::runtime_error("Projection node missing child");
            auto child = BuildOperatorTree(node->children[0].get(), table, join_table);
            Table *projection_join_table = join_table;
            if (node->children[0]->type == PhysicalPlanType::NESTED_LOOP_JOIN ||
                node->children[0]->type == PhysicalPlanType::HASH_JOIN)
            {
                projection_join_table = catalog_->GetTable(node->children[0]->right_table_name);
            }
            auto column_indices = ResolveProjectionIndices(node, table, projection_join_table);
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

        join_context_table_.reset();
        Optimizer optimizer;
        auto physical_plan = optimizer.BuildPhysicalPlan(select, catalog_);
        Table *right = nullptr;
        if (select->join_table.has_value())
        {
            right = catalog_->GetTable(*select->join_table);
        }
        return BuildOperatorTree(physical_plan.get(), table, right);
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
                if (select->join_table.has_value())
                {
                    Table *right = catalog_->GetTable(*select->join_table);
                    if (!right)
                        throw std::runtime_error("Join table not found: " + *select->join_table);
                    for (const auto &col : right->GetSchema().GetColumns())
                        result.column_names.push_back(col.name);
                }
            }
            else
            {
                result.column_names.clear();
                for (const auto &name : select->columns)
                {
                    result.column_names.push_back(StripQualifier(name));
                }
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
