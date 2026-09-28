#include "execution/executor.h"
#include "execution/evaluator.h"
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

    Value Executor::CoerceToColumn(const Value &value, const Column &column, const std::string &table)
    {
        const std::string where = table + "." + column.name;
        if (value.IsNull())
        {
            if (column.not_null || column.primary_key)
                throw std::runtime_error("NOT NULL constraint failed: " + where);
            return Value(column.type); // a bare NULL takes the column's type
        }

        // Numbers are stored as the column's numeric type, so a column (and
        // any index on it) holds one type
        std::optional<Value> converted = ConvertNumber(value, column.type);
        if (!converted)
            throw std::runtime_error("Cannot store " + value.ToString() + " in INTEGER column " + where);
        if (converted->GetType() != column.type)
            throw std::runtime_error("Type mismatch: cannot store " + value.ToString() + " in " +
                                     DataTypeName(column.type) + " column " + where);
        if (column.type == DataType::VARCHAR && column.length > 0 &&
            converted->GetAsString().size() > static_cast<size_t>(column.length))
            throw std::runtime_error("Value too long for VARCHAR(" + std::to_string(column.length) +
                                     ") column " + where);
        return *converted;
    }

    Value Executor::ResolveColumnValue(const ColumnExpression &col, const Tuple *tuple, Table *table) const
    {
        if (!tuple || !table)
            throw std::runtime_error("Column reference without tuple context");
        const size_t dot = col.name.find('.');
        int idx = -1;
        if (dot != std::string::npos)
        {
            const std::string qualifier = col.name.substr(0, dot);
            const std::string unqualified = col.name.substr(dot + 1);

            // Join context stores qualified column names directly.
            if (table->GetName() == "__join_context__")
            {
                idx = table->GetColumnIndex(col.name);
                if (idx < 0)
                {
                    throw std::runtime_error("Unknown column: " + col.name);
                }
                return tuple->GetValue(static_cast<size_t>(idx));
            }

            if (qualifier != table->GetName())
            {
                throw std::runtime_error("Unknown table qualifier in column: " + col.name);
            }

            idx = table->GetColumnIndex(unqualified);
            if (idx < 0)
            {
                throw std::runtime_error("Unknown column: " + col.name);
            }
            return tuple->GetValue(static_cast<size_t>(idx));
        }

        idx = table->GetColumnIndex(col.name);
        if (idx < 0)
        {
            const std::string stripped = StripQualifier(col.name);
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
                    throw std::runtime_error("Ambiguous column: " + col.name);
                }
                matched_idx = static_cast<int>(i);
            }
            idx = matched_idx;
        }
        if (idx < 0)
            throw std::runtime_error("Unknown column: " + col.name);
        return tuple->GetValue(static_cast<size_t>(idx));
    }

    Value Executor::EvaluateExpr(const Expression *expr, const Tuple *tuple, Table *table) const
    {
        return EvaluateExpression(expr, [&](const ColumnExpression &col)
                                  { return ResolveColumnValue(col, tuple, table); });
    }

    ExecutionResult Executor::Execute(Statement *stmt)
    {
        // Subquery results last for one statement: the data may change after
        subquery_cache_.clear();
        SubqueryScope subqueries([this](const SubqueryExpression &subquery, const ColumnResolver &resolve_outer)
                                 { return RunSubquery(subquery, resolve_outer); });
        return ExecuteStatement(stmt);
    }

    Value Executor::RunSubquery(const SubqueryExpression &subquery, const ColumnResolver &resolve_outer)
    {
        // The enclosing row's values for the names the subquery borrows
        std::vector<std::pair<std::string, Value>> outer;
        std::string key;
        for (const std::string &name : OuterReferences(*subquery.select, catalog_))
        {
            outer.emplace_back(name, resolve_outer(ColumnExpression(name)));
            AppendGroupKey(outer.back().second, &key);
        }

        auto cached = subquery_cache_.find({&subquery, key});
        if (cached == subquery_cache_.end())
        {
            std::unique_ptr<SelectStatement> bound = BindOuterReferences(*subquery.select, catalog_, outer);
            Executor inner(catalog_);
            ExecutionResult result = inner.Execute(bound.get());
            if (!result.success)
                throw std::runtime_error(result.message);
            ++subquery_runs_;
            subquery_runs_ += inner.SubqueryRuns();
            cached = subquery_cache_
                         .emplace(std::make_pair(&subquery, key),
                                  SubqueryResult{result.column_names.size(), std::move(result.tuples)})
                         .first;
        }
        const SubqueryResult &rows = cached->second;

        if (subquery.kind == SubqueryExpression::Kind::EXISTS)
            return Value(rows.rows.empty() == subquery.negated);
        if (rows.columns != 1)
            throw std::runtime_error("Subquery must return exactly one column, not " + std::to_string(rows.columns));
        if (subquery.kind == SubqueryExpression::Kind::SCALAR)
        {
            if (rows.rows.size() > 1)
                throw std::runtime_error("Scalar subquery returned more than one row");
            return rows.rows.empty() ? Value() : rows.rows[0].GetValue(0);
        }
        std::vector<Value> values;
        values.reserve(rows.rows.size());
        for (const Tuple &row : rows.rows)
            values.push_back(row.GetValue(0));
        const Value found = InValues(EvaluateExpression(subquery.operand.get(), resolve_outer), values);
        return subquery.negated ? EvaluateUnaryOp(TokenType::NOT, found) : found;
    }

    ExecutionResult Executor::ExecuteStatement(Statement *stmt)
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
        case StatementType::EXPLAIN_STMT:
            return ExecuteExplain(static_cast<ExplainStatement *>(stmt));
        case StatementType::ANALYZE_STMT:
        {
            const auto *analyze = static_cast<AnalyzeStatement *>(stmt);
            std::vector<std::string> tables{analyze->table};
            if (analyze->table.empty())
                tables = catalog_->GetTableNames();
            try
            {
                for (const auto &name : tables)
                {
                    if (!catalog_->Analyze(name))
                        return {false, "Table not found: " + name, {}, {}};
                }
            }
            catch (const std::exception &e)
            {
                return {false, e.what(), {}, {}};
            }
            return {true, "Analyzed " + std::to_string(tables.size()) + " table(s).", {}, {}};
        }
        case StatementType::TRANSACTION_STMT:
            return {false, "BEGIN / COMMIT / ROLLBACK must be run through a Session", {}, {}};
        default:
            return {false, "Unsupported statement type", {}, {}};
        }
    }

    Table *Executor::MaterializeOperatorToTable(std::unique_ptr<Operator> op, Table *source_table)
    {
        auto materialized = std::make_unique<Table>(source_table->GetName(), source_table->GetSchema());
        op->Open();
        Tuple row;
        while (op->Next(&row))
            materialized->Insert(row);
        op->Close();
        materialized_tables_.push_back(std::move(materialized));
        return materialized_tables_.back().get();
    }

    Table *Executor::JoinedContext(size_t count)
    {
        if (count == 0 || count > scope_.Size())
            throw std::logic_error("Plan node covers no known relations");
        if (count == 1)
            return RelationContext(0);
        if (joined_contexts_.size() <= count)
            joined_contexts_.resize(count + 1);
        if (!joined_contexts_[count])
        {
            std::vector<Column> columns;
            for (size_t r = 0; r < count; ++r)
            {
                const Scope::Relation &relation = scope_.Relations()[r];
                for (const auto &col : relation.schema->GetSchema().GetColumns())
                    columns.emplace_back(relation.name + "." + col.name, col.type, col.length);
            }
            joined_contexts_[count] = std::make_unique<Table>("__join_context__", Schema(std::move(columns)));
        }
        return joined_contexts_[count].get();
    }

    Table *Executor::RelationContext(size_t relation)
    {
        const Scope::Relation &r = scope_.Relations().at(relation);
        if (r.name == r.table)
            return r.schema;
        if (relation_contexts_.size() <= relation)
            relation_contexts_.resize(relation + 1);
        if (!relation_contexts_[relation])
            relation_contexts_[relation] = std::make_unique<Table>(r.name, r.schema->GetSchema());
        return relation_contexts_[relation].get();
    }

    Table *Executor::RowContext(const PhysicalPlanNode *node)
    {
        if (node->over_aggregate)
        {
            if (!aggregate_context_table_)
                throw std::logic_error("Aggregate output used before the aggregate was built");
            return aggregate_context_table_.get();
        }
        if (node->relation >= 0)
            return RelationContext(static_cast<size_t>(node->relation));
        return JoinedContext(node->relation_count);
    }

    Table *Executor::JoinRightInput(const PhysicalPlanNode *access_path, const Scope::Relation &relation)
    {
        if (access_path->type == PhysicalPlanType::SEQ_SCAN)
            return relation.schema;
        return MaterializeOperatorToTable(BuildOperatorTree(access_path), relation.schema);
    }

    std::unique_ptr<Operator> Executor::BuildOperatorTree(const PhysicalPlanNode *node)
    {
        if (node == nullptr)
            throw std::runtime_error("Null physical plan node");

        switch (node->type)
        {
        case PhysicalPlanType::SEQ_SCAN:
            return std::make_unique<SeqScan>(scope_.Relations().at(static_cast<size_t>(node->relation)).schema);
        case PhysicalPlanType::INDEX_SCAN:
        {
            Table *table = scope_.Relations().at(static_cast<size_t>(node->relation)).schema;
            BTree *index = catalog_->GetIndex(node->table_name, node->index_column);
            if (!index)
                throw std::runtime_error("Expected index not found on " + node->table_name + "." + node->index_column);
            if (node->is_point_lookup)
            {
                if (!node->point_key.has_value())
                    throw std::runtime_error("Point lookup index scan missing key");
                return std::make_unique<IndexScan>(table, index, *node->point_key);
            }
            return std::make_unique<IndexScan>(table, index, node->low_key, node->low_inclusive, node->high_key,
                                               node->high_inclusive);
        }
        case PhysicalPlanType::NESTED_LOOP_JOIN:
        case PhysicalPlanType::HASH_JOIN:
        case PhysicalPlanType::INDEX_NESTED_LOOP_JOIN:
        {
            const size_t j = node->relation_count - 1; // the right relation
            const Scope::Relation &right = scope_.Relations().at(j);
            JoinOutput output;
            output.type = node->join_type;
            output.residual = node->join_residual;
            output.context = JoinedContext(node->relation_count);
            output.right_schema = &right.schema->GetSchema();
            const PhysicalPlanNode *left_input = node->children.at(0).get();
            const PhysicalPlanNode *right_input = node->children.at(1).get();

            if (node->type == PhysicalPlanType::NESTED_LOOP_JOIN)
                return std::make_unique<NestedLoopJoin>(BuildOperatorTree(left_input), JoinRightInput(right_input, right),
                                                        std::move(output));
            if (node->type == PhysicalPlanType::HASH_JOIN)
                return std::make_unique<HashJoin>(BuildOperatorTree(left_input), JoinRightInput(right_input, right),
                                                  node->left_key, node->right_key, std::move(output),
                                                  node->join_build_right);

            // Index join: the probed side is read only through its index
            if (node->join_outer_is_left)
            {
                const std::string &column = right.schema->GetSchema().GetColumn(node->right_key).name;
                BTree *index = catalog_->GetIndex(right.table, column);
                if (index == nullptr || right_input->type != PhysicalPlanType::SEQ_SCAN)
                    throw std::logic_error("Index join needs an unfiltered, indexed right table");
                return std::make_unique<IndexNestedLoopJoin>(
                    BuildOperatorTree(left_input), right.schema, index, node->left_key,
                    right.schema->GetSchema().GetColumn(node->right_key).type, std::move(output), true);
            }
            const Scope::Relation &left = scope_.Relations().at(0);
            const std::string &column = left.schema->GetSchema().GetColumn(node->left_key).name;
            BTree *index = catalog_->GetIndex(left.table, column);
            if (index == nullptr || left_input->type != PhysicalPlanType::SEQ_SCAN || j != 1)
                throw std::logic_error("Index join needs an unfiltered, indexed left table");
            return std::make_unique<IndexNestedLoopJoin>(BuildOperatorTree(right_input), left.schema, index,
                                                         node->right_key,
                                                         left.schema->GetSchema().GetColumn(node->left_key).type,
                                                         std::move(output), false);
        }
        case PhysicalPlanType::FILTER:
        {
            auto child = BuildOperatorTree(node->children.at(0).get());
            return std::make_unique<Filter>(std::move(child), node->predicate, RowContext(node));
        }
        case PhysicalPlanType::SORT:
        {
            auto child = BuildOperatorTree(node->children.at(0).get());
            // Rows below the projection: a table's, joined rows, or an
            // aggregate's output
            return std::make_unique<Sort>(std::move(child), node->sort_keys, node->sort_descending,
                                          RowContext(node));
        }
        case PhysicalPlanType::AGGREGATE:
        {
            auto child = BuildOperatorTree(node->children.at(0).get());
            Table *input = RowContext(node);
            std::vector<Column> columns;
            for (const auto &name : node->aggregate_columns)
                columns.emplace_back(name, DataType::INTEGER, 0); // the type is not used
            aggregate_context_table_ = std::make_unique<Table>("__aggregate__", Schema(std::move(columns)));
            return std::make_unique<HashAggregate>(std::move(child), node->group_keys, node->aggregates, input);
        }
        case PhysicalPlanType::DISTINCT:
            return std::make_unique<Distinct>(BuildOperatorTree(node->children.at(0).get()));
        case PhysicalPlanType::LIMIT:
            return std::make_unique<Limit>(BuildOperatorTree(node->children.at(0).get()), node->limit, node->offset);
        case PhysicalPlanType::PROJECTION:
        {
            auto child = BuildOperatorTree(node->children.at(0).get());
            Table *context = RowContext(node);
            if (node->compute_projection)
                return std::make_unique<ExpressionProjection>(std::move(child), node->projected_exprs, context);
            std::vector<int> column_indices;
            if (!node->project_all)
            {
                for (const auto &name : node->projected_columns)
                {
                    const int idx = FindColumnIndex(*context, name);
                    if (idx < 0)
                        throw std::runtime_error("Unknown column: " + name);
                    column_indices.push_back(idx);
                }
            }
            return std::make_unique<Projection>(std::move(child), std::move(column_indices), node->project_all);
        }
        }
        throw std::runtime_error("Unknown physical plan node type");
    }

    std::unique_ptr<Operator> Executor::BuildPlan(SelectStatement *select)
    {
        materialized_tables_.clear();
        joined_contexts_.clear();
        relation_contexts_.clear();
        aggregate_context_table_.reset();
        scope_ = Scope::ForSelect(*select, catalog_);
        Optimizer optimizer;
        // Operators may point into the plan (e.g. sort keys the planner
        // created), so it lives as long as the executor
        physical_plan_ = optimizer.BuildPhysicalPlan(select, catalog_);
        // The planner may join the tables in another order than written
        if (!physical_plan_->relation_order.empty())
            scope_ = scope_.Reordered(physical_plan_->relation_order);
        return BuildOperatorTree(physical_plan_.get());
    }

    ExecutionResult Executor::ExecuteSelect(SelectStatement *select)
    {
        ExecutionResult result;
        try
        {
            auto plan = BuildPlan(select);

            if (select->select_star)
            {
                // Columns in FROM order, whatever order the joins ran in
                const Scope written = Scope::ForSelect(*select, catalog_);
                for (const auto &relation : written.Relations())
                {
                    for (const auto &col : relation.schema->GetSchema().GetColumns())
                        result.column_names.push_back(col.name);
                }
            }
            else
            {
                // An alias, else the column's name, else the expression's SQL
                for (const auto &item : select->items)
                {
                    if (!item.alias.empty())
                        result.column_names.push_back(item.alias);
                    else if (item.expr->GetType() == ExpressionType::COLUMN_REF)
                        result.column_names.push_back(
                            StripQualifier(static_cast<const ColumnExpression *>(item.expr.get())->name));
                    else
                        result.column_names.push_back(ExpressionToSQL(item.expr.get()));
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
            size_t primary_keys = 0;
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
                for (const auto &existing : columns)
                {
                    if (existing.name == cd.name)
                        throw std::runtime_error("Duplicate column name: " + cd.name);
                }
                Column column(cd.name, dt, cd.length);
                column.primary_key = cd.primary_key;
                column.not_null = cd.not_null || cd.primary_key;
                column.unique = cd.unique || cd.primary_key;
                if (cd.primary_key && ++primary_keys > 1)
                    throw std::runtime_error("Table '" + create->table + "' has more than one PRIMARY KEY");
                if (cd.default_value)
                {
                    // Store the default as the column will store it
                    try
                    {
                        column.default_value = CoerceToColumn(*cd.default_value, column, create->table);
                    }
                    catch (const std::exception &e)
                    {
                        throw std::runtime_error("Invalid DEFAULT for column " + cd.name + ": " + e.what());
                    }
                }
                columns.push_back(std::move(column));
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
            const Schema &schema = table->GetSchema();
            const size_t col_count = schema.GetColumnCount();

            // Which table column each VALUES position fills
            std::vector<size_t> targets;
            if (insert->columns.empty())
            {
                for (size_t c = 0; c < col_count; ++c)
                    targets.push_back(c);
            }
            else
            {
                std::vector<bool> seen(col_count, false);
                for (const auto &name : insert->columns)
                {
                    const int idx = table->GetColumnIndex(name);
                    if (idx < 0)
                        throw std::runtime_error("Unknown column: " + name);
                    if (seen[static_cast<size_t>(idx)])
                        throw std::runtime_error("Column listed twice: " + name);
                    seen[static_cast<size_t>(idx)] = true;
                    targets.push_back(static_cast<size_t>(idx));
                }
            }

            size_t rows_inserted = 0;
            for (auto &row : insert->rows)
            {
                if (row.size() != targets.size())
                    throw std::runtime_error("Column count mismatch: expected " +
                                             std::to_string(targets.size()) + ", got " +
                                             std::to_string(row.size()));

                // Columns the statement does not mention take their default
                std::vector<Value> values;
                values.reserve(col_count);
                for (size_t c = 0; c < col_count; ++c)
                {
                    const Column &column = schema.GetColumn(c);
                    values.push_back(column.default_value ? *column.default_value : Value(column.type));
                }
                for (size_t i = 0; i < targets.size(); ++i)
                    values[targets[i]] = EvaluateExpr(row[i].get());
                for (size_t c = 0; c < col_count; ++c)
                    values[c] = CoerceToColumn(values[c], schema.GetColumn(c), table->GetName());

                catalog_->InsertRow(table, Tuple(std::move(values)));
                rows_inserted++;
            }

            result.success = true;
            result.message = std::to_string(rows_inserted) + " row(s) inserted.";
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

            std::vector<RID> to_delete;

            for (auto it = table->begin(); it != table->end(); ++it)
            {
                if (!del->where)
                {
                    to_delete.push_back(it.GetRID());
                }
                else
                {
                    if (IsTrue(EvaluateExpr(del->where.get(), &*it, table)))
                        to_delete.push_back(it.GetRID());
                }
            }

            for (const RID &rid : to_delete)
            {
                if (!catalog_->DeleteRow(table, rid))
                    throw std::runtime_error("Failed to delete row " + rid.ToString());
            }
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

            std::vector<std::pair<RID, Tuple>> updates; // (location, new tuple)

            for (auto it = table->begin(); it != table->end(); ++it)
            {
                const Tuple &existing_tuple = *it;
                bool matches = true;
                if (update->where)
                {
                    matches = IsTrue(EvaluateExpr(update->where.get(), &existing_tuple, table));
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
                        new_values[static_cast<size_t>(col_idx)] = CoerceToColumn(
                            EvaluateExpr(assign.second.get(), &existing_tuple, table),
                            table->GetSchema().GetColumn(static_cast<size_t>(col_idx)), table->GetName());
                    }

                    updates.push_back({it.GetRID(), Tuple(std::move(new_values))});
                }
            }

            // Apply all updates
            for (const auto &update_pair : updates)
            {
                if (!catalog_->UpdateRow(table, update_pair.first, update_pair.second))
                    throw std::runtime_error("Failed to update row " + update_pair.first.ToString());
            }

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
        bool created = false;
        try
        {
            created = catalog_->CreateIndex(create->index_name, create->table, create->column, create->unique);
        }
        catch (const std::exception &e)
        {
            result.success = false;
            result.message = "Failed to create index '" + create->index_name + "': " + e.what();
            return result;
        }
        if (!created)
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
        try
        {
            if (!catalog_->DropTable(drop->table))
                return {false, "Table '" + drop->table + "' does not exist.", {}, {}};
        }
        catch (const std::exception &e)
        {
            return {false, e.what(), {}, {}};
        }
        return {true, "Table '" + drop->table + "' dropped.", {}, {}};
    }

    ExecutionResult Executor::ExecuteExplain(ExplainStatement *explain)
    {
        ExecutionResult result;
        try
        {
            Optimizer optimizer;
            auto physical_plan = optimizer.BuildPhysicalPlan(explain->select.get(), catalog_);
            result.success = true;
            result.message = optimizer.ExplainPhysicalPlan(physical_plan.get());
        }
        catch (const std::exception &e)
        {
            result.success = false;
            result.message = e.what();
        }
        return result;
    }

} // namespace sql
