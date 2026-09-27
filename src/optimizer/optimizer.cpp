#include "optimizer/optimizer.h"
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <sstream>

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
            // Comparisons with NULL are never true, and NULLs are not indexed
            if (lit_expr->value.IsNull())
            {
                return false;
            }
            *column_name = col_expr->name;
            *op = bin->op;
            *literal_value = lit_expr->value;
            return true;
        }
        // What EXPLAIN shows for each SELECT item: the column name when the
        // list is plain columns (the executor looks these up), else SQL text
        std::vector<std::string> ProjectionLabels(const SelectStatement &select)
        {
            if (!select.HasComputedItems())
                return select.columns;
            std::vector<std::string> labels;
            for (const auto &item : select.items)
                labels.push_back(ExpressionToSQL(item.expr.get()) + (item.alias.empty() ? "" : " AS " + item.alias));
            return labels;
        }

        // GROUP BY, HAVING or an aggregate function anywhere it may appear
        bool IsAggregateQuery(const SelectStatement &select)
        {
            if (!select.group_by.empty() || select.having)
                return true;
            for (const auto &item : select.items)
            {
                if (ContainsAggregate(item.expr.get()))
                    return true;
            }
            for (const auto &order : select.order_by)
            {
                if (ContainsAggregate(order.expr.get()))
                    return true;
            }
            return false;
        }

        // col BETWEEN low AND high with literal bounds of the column's kind
        bool IsIndexableRange(const Expression *expr, std::string *column_name, Value *low, Value *high)
        {
            if (expr == nullptr || expr->GetType() != ExpressionType::BETWEEN)
                return false;
            const auto *between = static_cast<const BetweenExpression *>(expr);
            if (between->negated || between->operand->GetType() != ExpressionType::COLUMN_REF ||
                between->low->GetType() != ExpressionType::LITERAL || between->high->GetType() != ExpressionType::LITERAL)
                return false;
            *low = static_cast<const LiteralExpression *>(between->low.get())->value;
            *high = static_cast<const LiteralExpression *>(between->high.get())->value;
            if (low->IsNull() || high->IsNull())
                return false;
            *column_name = static_cast<const ColumnExpression *>(between->operand.get())->name;
            return true;
        }

        // The AND-ed parts of a condition: a AND (b AND c) gives a, b, c
        void SplitConjuncts(const Expression *expr, std::vector<const Expression *> *out)
        {
            if (expr == nullptr)
                return;
            if (expr->GetType() == ExpressionType::BINARY_OP &&
                static_cast<const BinaryExpression *>(expr)->op == TokenType::AND)
            {
                const auto *bin = static_cast<const BinaryExpression *>(expr);
                SplitConjuncts(bin->left.get(), out);
                SplitConjuncts(bin->right.get(), out);
                return;
            }
            out->push_back(expr);
        }

        // The conditions AND-ed together: the condition itself when there
        // is one, else a new expression kept alive by `owner`
        const Expression *Conjunction(const std::vector<const Expression *> &conditions, PhysicalPlanNode *owner)
        {
            if (conditions.size() == 1)
                return conditions[0];
            auto copy = [](const Expression *e)
            { return RewriteExpression(e, [](const Expression *) { return std::unique_ptr<Expression>(); }); };
            std::unique_ptr<Expression> all = copy(conditions[0]);
            for (size_t i = 1; i < conditions.size(); ++i)
                all = std::make_unique<BinaryExpression>(std::move(all), TokenType::AND, copy(conditions[i]));
            owner->owned_exprs.push_back(std::move(all));
            return owner->owned_exprs.back().get();
        }

        // Every column reference in the expression, depth first
        void ForEachColumn(const Expression *expr, const std::function<void(const std::string &)> &visit)
        {
            if (expr == nullptr)
                return;
            if (expr->GetType() == ExpressionType::COLUMN_REF)
                visit(static_cast<const ColumnExpression *>(expr)->name);
            for (const Expression *child : ExpressionChildren(expr))
                ForEachColumn(child, visit);
        }

        // ForEachColumn, plus the enclosing-query names used by subqueries
        void ForEachColumnUsed(const Expression *expr, Catalog *catalog,
                               const std::function<void(const std::string &)> &visit)
        {
            if (expr == nullptr)
                return;
            if (expr->GetType() == ExpressionType::COLUMN_REF)
                visit(static_cast<const ColumnExpression *>(expr)->name);
            if (expr->GetType() == ExpressionType::SUBQUERY && catalog != nullptr)
            {
                for (const std::string &name :
                     OuterReferences(*static_cast<const SubqueryExpression *>(expr)->select, catalog))
                    visit(name);
            }
            for (const Expression *child : ExpressionChildren(expr))
                ForEachColumnUsed(child, catalog, visit);
        }

        bool IsSelectAlias(const SelectStatement &select, const std::string &name)
        {
            for (const auto &item : select.items)
            {
                if (!item.alias.empty() && item.alias == name)
                    return true;
            }
            return false;
        }

        bool ResolvedInChain(const std::vector<const Scope *> &chain, const SelectStatement &select,
                             const std::string &name)
        {
            if (IsSelectAlias(select, name))
                return true;
            for (const Scope *scope : chain)
            {
                if (scope->CanResolve(name))
                    return true;
            }
            return false;
        }

        // Calls on_outer for each name in `select` (and the subqueries in
        // it) that no query in the chain resolves
        void WalkOuterReferences(const SelectStatement &select, Catalog *catalog, std::vector<const Scope *> *chain,
                                 const std::function<void(const std::string &)> &on_outer)
        {
            const Scope scope = Scope::ForSelect(select, catalog);
            chain->push_back(&scope);
            std::function<void(const Expression *)> walk = [&](const Expression *expr)
            {
                if (expr == nullptr)
                    return;
                if (expr->GetType() == ExpressionType::COLUMN_REF)
                {
                    const std::string &name = static_cast<const ColumnExpression *>(expr)->name;
                    if (!ResolvedInChain(*chain, select, name))
                        on_outer(name);
                }
                if (expr->GetType() == ExpressionType::SUBQUERY)
                    WalkOuterReferences(*static_cast<const SubqueryExpression *>(expr)->select, catalog, chain,
                                        on_outer);
                for (const Expression *child : ExpressionChildren(expr))
                    walk(child);
            };
            for (const Expression *expr : SelectExpressions(select))
                walk(expr);
            chain->pop_back();
        }

        std::unique_ptr<SelectStatement> BindInChain(const SelectStatement &select, Catalog *catalog,
                                                     std::vector<const Scope *> *chain,
                                                     const std::vector<std::pair<std::string, Value>> &values)
        {
            const Scope scope = Scope::ForSelect(select, catalog);
            chain->push_back(&scope);
            std::function<std::unique_ptr<Expression>(const Expression *)> replace =
                [&](const Expression *node) -> std::unique_ptr<Expression>
            {
                if (node->GetType() == ExpressionType::COLUMN_REF)
                {
                    const std::string &name = static_cast<const ColumnExpression *>(node)->name;
                    if (ResolvedInChain(*chain, select, name))
                        return nullptr;
                    for (const auto &[outer_name, value] : values)
                    {
                        if (outer_name == name)
                            return std::make_unique<LiteralExpression>(value);
                    }
                    return nullptr;
                }
                if (node->GetType() == ExpressionType::SUBQUERY)
                {
                    const auto *subquery = static_cast<const SubqueryExpression *>(node);
                    return std::make_unique<SubqueryExpression>(
                        subquery->kind, subquery->negated, RewriteExpression(subquery->operand.get(), replace),
                        BindInChain(*subquery->select, catalog, chain, values));
                }
                return nullptr;
            };
            auto bound = CloneSelect(select, replace);
            chain->pop_back();
            return bound;
        }

        // Qualified names must name a table in FROM, and ON conditions may
        // only use the tables joined so far. (Unqualified names elsewhere
        // may be SELECT aliases, so evaluation reports those.)
        void CheckColumnNames(const SelectStatement &select, const Scope &scope)
        {
            auto check_qualified = [&](const Expression *expr)
            {
                ForEachColumn(expr, [&](const std::string &name)
                              {
                    if (name.find('.') != std::string::npos)
                        scope.Resolve(name); });
            };
            for (const auto &item : select.items)
                check_qualified(item.expr.get());
            check_qualified(select.where.get());
            for (const auto &key : select.group_by)
                check_qualified(key.get());
            check_qualified(select.having.get());
            for (const auto &order : select.order_by)
                check_qualified(order.expr.get());
            for (size_t j = 0; j < select.joins.size(); ++j)
            {
                if (ContainsAggregate(select.joins[j].on.get()))
                    throw std::runtime_error("Aggregate functions are not allowed in ON");
                ForEachColumn(select.joins[j].on.get(), [&](const std::string &name)
                              { scope.Resolve(name, j + 2); });
            }
        }

        // An index scan of one relation for a condition on an indexed
        // column, or null
        std::unique_ptr<PhysicalPlanNode> IndexAccess(const Scope::Relation &relation, size_t index,
                                                      const Expression *condition, Catalog *catalog)
        {
            std::string column_name;
            TokenType op = TokenType::ILLEGAL;
            Value literal;
            Value low, high;
            const bool comparison = IsIndexableComparison(condition, &column_name, &op, &literal);
            if (!comparison && !IsIndexableRange(condition, &column_name, &low, &high))
                return nullptr;

            const std::string column = StripQualifier(column_name);
            const int col_idx = relation.schema->GetColumnIndex(column);
            if (col_idx < 0 || catalog->GetIndex(relation.table, column) == nullptr)
                return nullptr;
            const DataType type = relation.schema->GetSchema().GetColumn(static_cast<size_t>(col_idx)).type;

            auto scan = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::INDEX_SCAN);
            scan->table_name = relation.table;
            scan->relation = static_cast<int>(index);
            scan->index_column = column;
            if (!comparison)
            {
                if (type != low.GetType() || type != high.GetType())
                    return nullptr;
                scan->low_key = low;
                scan->high_key = high;
                return scan;
            }
            if (type != literal.GetType())
                return nullptr;
            switch (op)
            {
            case TokenType::EQ:
                scan->is_point_lookup = true;
                scan->point_key = literal;
                break;
            case TokenType::GT:
            case TokenType::GEQ:
                scan->low_key = literal;
                scan->low_inclusive = op == TokenType::GEQ;
                break;
            default: // LT, LEQ
                scan->high_key = literal;
                scan->high_inclusive = op == TokenType::LEQ;
                break;
            }
            return scan;
        }

        std::string RelationLabel(const Scope::Relation &relation)
        {
            return relation.name == relation.table ? relation.table : relation.table + " AS " + relation.name;
        }
    } // namespace

    std::vector<std::string> OuterReferences(const SelectStatement &subquery, Catalog *catalog)
    {
        std::vector<std::string> names;
        std::vector<const Scope *> chain;
        WalkOuterReferences(subquery, catalog, &chain, [&](const std::string &name)
                            {
            if (std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(name); });
        return names;
    }

    std::unique_ptr<SelectStatement> BindOuterReferences(const SelectStatement &subquery, Catalog *catalog,
                                                         const std::vector<std::pair<std::string, Value>> &values)
    {
        std::vector<const Scope *> chain;
        return BindInChain(subquery, catalog, &chain, values);
    }

    Scope Scope::ForSelect(const SelectStatement &select, Catalog *catalog)
    {
        Scope scope;
        scope.catalog_ = catalog;
        auto add = [&](const std::string &table, const std::string &alias)
        {
            Table *schema = catalog->GetTable(table);
            if (schema == nullptr)
                throw std::runtime_error("Table not found: " + table);
            const std::string name = alias.empty() ? table : alias;
            for (const auto &existing : scope.relations_)
            {
                if (existing.name == name)
                    throw std::runtime_error("Table name '" + name + "' specified more than once; use an alias");
            }
            scope.relations_.push_back({name, table, schema});
        };
        add(select.table, select.table_alias);
        for (const auto &join : select.joins)
            add(join.right.table, join.right.alias);
        if (scope.relations_.size() > 64)
            throw std::runtime_error("A query can join at most 64 tables");
        return scope;
    }

    size_t Scope::Offset(size_t relation) const
    {
        size_t offset = 0;
        for (size_t i = 0; i < relation; ++i)
            offset += relations_[i].schema->GetSchema().GetColumnCount();
        return offset;
    }

    Scope::Resolved Scope::Resolve(const std::string &name, size_t count) const
    {
        count = std::min(count, relations_.size());
        const size_t dot = name.find('.');
        if (dot != std::string::npos)
        {
            const std::string qualifier = name.substr(0, dot);
            for (size_t i = 0; i < count; ++i)
            {
                if (relations_[i].name != qualifier)
                    continue;
                const int idx = relations_[i].schema->GetColumnIndex(name.substr(dot + 1));
                if (idx < 0)
                    throw std::runtime_error("Unknown column: " + name);
                return {i, static_cast<size_t>(idx)};
            }
            throw std::runtime_error("Unknown table qualifier in column: " + name);
        }
        std::optional<Resolved> found;
        for (size_t i = 0; i < count; ++i)
        {
            const int idx = relations_[i].schema->GetColumnIndex(name);
            if (idx < 0)
                continue;
            if (found)
                throw std::runtime_error("Ambiguous column: " + name);
            found = Resolved{i, static_cast<size_t>(idx)};
        }
        if (!found)
            throw std::runtime_error("Unknown column: " + name);
        return *found;
    }

    bool Scope::CanResolve(const std::string &name, size_t count) const
    {
        try
        {
            Resolve(name, count);
            return true;
        }
        catch (const std::runtime_error &)
        {
            return false;
        }
    }

    std::string Scope::Canonical(const std::string &name) const
    {
        if (!CanResolve(name))
            return name; // evaluation reports it
        const Resolved r = Resolve(name);
        return relations_[r.relation].name + "." +
               relations_[r.relation].schema->GetSchema().GetColumn(r.column).name;
    }

    std::optional<uint64_t> Scope::RelationsOf(const Expression *expr, size_t count) const
    {
        uint64_t relations = 0;
        bool resolved = true;
        ForEachColumnUsed(expr, catalog_, [&](const std::string &name)
                          {
            if (CanResolve(name, count))
                relations |= uint64_t{1} << Resolve(name, count).relation;
            else
                resolved = false; });
        if (!resolved)
            return std::nullopt;
        return relations;
    }

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

        if (IsAggregateQuery(*select))
        {
            auto aggregate = std::make_unique<LogicalPlanNode>(LogicalPlanType::AGGREGATE);
            for (const auto &key : select->group_by)
                aggregate->group_labels.push_back(ExpressionToSQL(key.get()));
            aggregate->children.push_back(std::move(current));
            current = std::move(aggregate);
            if (select->having)
            {
                auto having = std::make_unique<LogicalPlanNode>(LogicalPlanType::FILTER);
                having->predicate = select->having.get();
                having->children.push_back(std::move(current));
                current = std::move(having);
            }
        }

        auto projection = std::make_unique<LogicalPlanNode>(LogicalPlanType::PROJECTION);
        projection->project_all = select->select_star;
        projection->projected_columns = ProjectionLabels(*select);
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
        case LogicalPlanType::AGGREGATE:
            out << pad << "Aggregate(group=[";
            for (size_t i = 0; i < node->group_labels.size(); ++i)
                out << (i ? ", " : "") << node->group_labels[i];
            out << "])";
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
        const Scope scope = Scope::ForSelect(*select, catalog);
        const size_t n = scope.Size();
        if (ContainsAggregate(select->where.get()))
            throw std::runtime_error("Aggregate functions are not allowed in WHERE");
        CheckColumnNames(*select, scope);

        // How each table joins the ones before it (tables join left to
        // right, in the order written)
        struct JoinStep
        {
            PhysicalPlanType algorithm = PhysicalPlanType::NESTED_LOOP_JOIN;
            std::vector<const Expression *> residual;
            const Expression *key = nullptr; // left.col = right.col used by hash / index joins
            Scope::Resolved left_key{0, 0};
            Scope::Resolved right_key{0, 0};
            bool build_right = true;
            bool outer_is_left = true;
        };
        std::vector<JoinStep> steps(n);
        // A WHERE condition on one table can filter it before the joins,
        // unless that table is reached through an index (every row of it is
        // probed) or is NULL-padded by a LEFT JOIN (the condition must also
        // see the padded rows)
        std::vector<bool> can_push(n, true);
        size_t rows_so_far = scope.Relations()[0].schema->GetTupleCount();
        for (size_t j = 1; j < n; ++j)
        {
            const JoinClause &clause = select->joins[j - 1];
            const Scope::Relation &right = scope.Relations()[j];
            JoinStep &step = steps[j];
            std::vector<const Expression *> conditions;
            SplitConjuncts(clause.on.get(), &conditions);

            for (const Expression *condition : conditions)
            {
                if (condition->GetType() != ExpressionType::BINARY_OP)
                    continue;
                const auto *eq = static_cast<const BinaryExpression *>(condition);
                if (eq->op != TokenType::EQ || eq->left->GetType() != ExpressionType::COLUMN_REF ||
                    eq->right->GetType() != ExpressionType::COLUMN_REF)
                    continue;
                Scope::Resolved a = scope.Resolve(static_cast<const ColumnExpression *>(eq->left.get())->name, j + 1);
                Scope::Resolved b = scope.Resolve(static_cast<const ColumnExpression *>(eq->right.get())->name, j + 1);
                if (a.relation == j)
                    std::swap(a, b);
                if (b.relation == j && a.relation < j)
                {
                    step.key = condition;
                    step.left_key = a;
                    step.right_key = b;
                    break;
                }
            }

            const size_t right_rows = right.schema->GetTupleCount();
            if (step.key != nullptr)
            {
                const Scope::Relation &left = scope.Relations()[step.left_key.relation];
                const std::string &right_col = right.schema->GetSchema().GetColumn(step.right_key.column).name;
                const std::string &left_col = left.schema->GetSchema().GetColumn(step.left_key.column).name;
                if (catalog->GetIndex(right.table, right_col) != nullptr)
                {
                    step.algorithm = PhysicalPlanType::INDEX_NESTED_LOOP_JOIN;
                    can_push[j] = false;
                }
                else if (j == 1 && clause.type == JoinType::INNER && catalog->GetIndex(left.table, left_col) != nullptr)
                {
                    // Only the first table has an index: probe it for each
                    // row of the second
                    step.algorithm = PhysicalPlanType::INDEX_NESTED_LOOP_JOIN;
                    step.outer_is_left = false;
                    can_push[0] = false;
                }
                else if (rows_so_far + right_rows >= 16)
                {
                    step.algorithm = PhysicalPlanType::HASH_JOIN;
                    // Hash the smaller side; a LEFT join keeps every left
                    // row, so it streams them past the right side's table
                    step.build_right = clause.type == JoinType::LEFT || right_rows <= rows_so_far;
                }
            }
            for (const Expression *condition : conditions)
            {
                if (condition != step.key || step.algorithm == PhysicalPlanType::NESTED_LOOP_JOIN)
                    step.residual.push_back(condition);
            }
            if (clause.type == JoinType::LEFT)
                can_push[j] = false;
            rows_so_far += right_rows;
        }

        // Split WHERE into conditions on single tables (pushed down) and the
        // rest (checked on the joined rows)
        std::vector<const Expression *> where;
        SplitConjuncts(select->where.get(), &where);
        std::vector<std::vector<const Expression *>> pushed(n);
        std::vector<const Expression *> remaining;
        for (const Expression *condition : where)
        {
            const std::optional<uint64_t> relations = scope.RelationsOf(condition);
            if (relations && *relations != 0 && (*relations & (*relations - 1)) == 0)
            {
                size_t r = 0;
                while (!((*relations >> r) & 1U))
                    ++r;
                if (can_push[r])
                {
                    pushed[r].push_back(condition);
                    continue;
                }
            }
            remaining.push_back(condition);
        }

        // One table's rows, filtered by the conditions pushed down to it;
        // a condition on an indexed column becomes an index scan
        auto access_path = [&](size_t r) -> std::unique_ptr<PhysicalPlanNode>
        {
            const Scope::Relation &relation = scope.Relations()[r];
            std::unique_ptr<PhysicalPlanNode> path;
            for (const Expression *condition : pushed[r])
            {
                if (!path)
                    path = IndexAccess(relation, r, condition, catalog);
            }
            if (!path)
            {
                path = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::SEQ_SCAN);
                path->table_name = relation.table;
                path->relation = static_cast<int>(r);
            }
            if (!pushed[r].empty())
            {
                auto filter = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::FILTER);
                filter->table_name = relation.table;
                filter->relation = static_cast<int>(r);
                // The whole WHERE when it all applies here
                filter->predicate = pushed[r].size() == where.size() ? select->where.get()
                                                                     : Conjunction(pushed[r], filter.get());
                filter->children.push_back(std::move(path));
                path = std::move(filter);
            }
            return path;
        };

        std::unique_ptr<PhysicalPlanNode> current = access_path(0);
        for (size_t j = 1; j < n; ++j)
        {
            const JoinClause &clause = select->joins[j - 1];
            const Scope::Relation &right = scope.Relations()[j];
            JoinStep &step = steps[j];
            auto join = std::make_unique<PhysicalPlanNode>(step.algorithm);
            join->join_type = clause.type;
            join->table_name = scope.Relations()[0].table;
            join->right_table_name = right.table;
            join->right_label = RelationLabel(right);
            join->relation_count = j + 1;
            join->join_residual = step.residual;
            join->join_on_label = clause.on ? ExpressionToSQL(clause.on.get()) : "";
            if (step.key != nullptr)
            {
                join->join_key_label = ExpressionToSQL(step.key);
                join->left_key = scope.Offset(step.left_key.relation) + step.left_key.column;
                join->right_key = step.right_key.column;
            }
            join->join_build_right = step.build_right;
            join->join_outer_is_left = step.outer_is_left;
            join->children.push_back(std::move(current));
            join->children.push_back(access_path(j));
            current = std::move(join);
        }

        if (!remaining.empty())
        {
            auto filter = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::FILTER);
            filter->table_name = n > 1 ? "__join_context__" : select->table;
            filter->relation_count = n;
            filter->predicate = remaining.size() == where.size() ? select->where.get()
                                                                 : Conjunction(remaining, filter.get());
            filter->children.push_back(std::move(current));
            current = std::move(filter);
        }

        if (IsAggregateQuery(*select))
        {
            current = BuildAggregatePlan(*select, scope, std::move(current));
        }
        else
        {
            // ORDER BY sorts the rows before projection, so it can use
            // columns that are not selected
            if (!select->order_by.empty())
            {
                auto sort = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::SORT);
                sort->relation_count = n;
                ResolveSortKeys(*select, scope, sort.get());
                sort->children.push_back(std::move(current));
                current = std::move(sort);
            }

            auto projection = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::PROJECTION);
            projection->relation_count = n;
            projection->project_all = select->select_star;
            projection->projected_columns = ProjectionLabels(*select);
            if (select->HasComputedItems())
            {
                projection->compute_projection = true;
                for (const auto &item : select->items)
                    projection->projected_exprs.push_back(item.expr.get());
            }
            projection->children.push_back(std::move(current));
            current = std::move(projection);
        }

        if (select->distinct)
        {
            auto distinct = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::DISTINCT);
            distinct->children.push_back(std::move(current));
            current = std::move(distinct);
        }
        if (select->limit || select->offset > 0)
        {
            auto limit = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::LIMIT);
            limit->limit = select->limit;
            limit->offset = select->offset;
            limit->children.push_back(std::move(current));
            current = std::move(limit);
        }
        return current;
    }

    std::unique_ptr<PhysicalPlanNode> Optimizer::BuildAggregatePlan(const SelectStatement &select, const Scope &scope,
                                                                    std::unique_ptr<PhysicalPlanNode> input) const
    {
        if (select.select_star)
            throw std::runtime_error("SELECT * cannot be used with GROUP BY or aggregate functions");
        auto same_column = [&](const std::string &a, const std::string &b)
        { return scope.Canonical(a) == scope.Canonical(b); };

        auto aggregate = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::AGGREGATE);
        aggregate->relation_count = scope.Size();
        // Output columns are named by their SQL text (what EXPLAIN shows);
        // equal texts for different expressions get a suffix
        auto add_column = [&](const std::string &name)
        {
            std::string unique = name;
            for (int n = 2; std::find(aggregate->aggregate_columns.begin(), aggregate->aggregate_columns.end(),
                                      unique) != aggregate->aggregate_columns.end();
                 ++n)
                unique = name + " #" + std::to_string(n);
            aggregate->aggregate_columns.push_back(unique);
            return unique;
        };

        // Group keys first, each once. GROUP BY 2 names the second SELECT
        // item, and a name that is not a column may be an item's alias.
        for (const auto &group : select.group_by)
        {
            const Expression *key = group.get();
            if (key->GetType() == ExpressionType::LITERAL)
            {
                const Value &v = static_cast<const LiteralExpression *>(key)->value;
                if (!v.IsNull() && v.GetType() == DataType::INTEGER)
                {
                    const int64_t pos = v.GetAsInt();
                    if (pos < 1 || pos > static_cast<int64_t>(select.items.size()))
                        throw std::runtime_error("GROUP BY position " + std::to_string(pos) + " is out of range");
                    key = select.items[static_cast<size_t>(pos - 1)].expr.get();
                }
            }
            else if (key->GetType() == ExpressionType::COLUMN_REF)
            {
                const std::string &name = static_cast<const ColumnExpression *>(key)->name;
                const bool is_column = scope.CanResolve(name);
                for (const auto &item : select.items)
                {
                    if (!is_column && !item.alias.empty() && item.alias == name)
                        key = item.expr.get();
                }
            }
            if (ContainsAggregate(key))
                throw std::runtime_error("Aggregate functions are not allowed in GROUP BY");

            bool repeated = false;
            for (const Expression *existing : aggregate->group_keys)
                repeated = repeated || ExpressionsEqual(existing, key, same_column);
            if (!repeated)
            {
                aggregate->group_keys.push_back(key);
                add_column(ExpressionToSQL(key));
            }
        }
        const size_t key_count = aggregate->group_keys.size();

        // Rewrite an expression over the aggregate's output rows: aggregates
        // and group keys become references to their columns, and any other
        // column is an error. HAVING and ORDER BY may also name a SELECT
        // alias (not inside an alias's own expression).
        std::function<std::unique_ptr<Expression>(const Expression *, bool)> rewrite;
        rewrite = [&](const Expression *expr, bool allow_alias)
        {
            return RewriteExpression(expr, [&](const Expression *node) -> std::unique_ptr<Expression>
                                     {
                if (node->GetType() == ExpressionType::SUBQUERY)
                {
                    // Its body is a query of its own; only an IN operand
                    // belongs to the grouped rows
                    const auto *subquery = static_cast<const SubqueryExpression *>(node);
                    if (!OuterReferences(*subquery->select, scope.GetCatalog()).empty())
                        throw std::runtime_error(
                            "Correlated subqueries are not supported in the SELECT list, HAVING or ORDER BY of "
                            "a query with GROUP BY or aggregates");
                    return std::make_unique<SubqueryExpression>(
                        subquery->kind, subquery->negated,
                        subquery->operand ? rewrite(subquery->operand.get(), allow_alias) : nullptr,
                        CloneSelect(*subquery->select, [](const Expression *) { return std::unique_ptr<Expression>(); }));
                }
                if (node->GetType() == ExpressionType::AGGREGATE)
                {
                    const auto *call = static_cast<const AggregateExpression *>(node);
                    if (ContainsAggregate(call->argument.get()))
                        throw std::runtime_error("Aggregate function calls cannot be nested");
                    for (size_t i = 0; i < aggregate->aggregates.size(); ++i)
                    {
                        if (ExpressionsEqual(aggregate->aggregates[i], call, same_column))
                            return std::make_unique<ColumnExpression>(aggregate->aggregate_columns[key_count + i]);
                    }
                    aggregate->aggregates.push_back(call);
                    return std::make_unique<ColumnExpression>(add_column(ExpressionToSQL(call)));
                }
                for (size_t i = 0; i < key_count; ++i)
                {
                    if (ExpressionsEqual(aggregate->group_keys[i], node, same_column))
                        return std::make_unique<ColumnExpression>(aggregate->aggregate_columns[i]);
                }
                if (node->GetType() == ExpressionType::COLUMN_REF)
                {
                    const std::string &name = static_cast<const ColumnExpression *>(node)->name;
                    if (allow_alias)
                    {
                        for (const auto &item : select.items)
                        {
                            if (!item.alias.empty() && item.alias == name)
                                return rewrite(item.expr.get(), false);
                        }
                    }
                    throw std::runtime_error("Column '" + name +
                                             "' must appear in the GROUP BY clause or be used in an aggregate function");
                }
                return nullptr; });
        };

        auto projection = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::PROJECTION);
        projection->projected_columns = ProjectionLabels(select);
        projection->compute_projection = true;
        projection->over_aggregate = true;
        for (const auto &item : select.items)
        {
            projection->owned_exprs.push_back(rewrite(item.expr.get(), false));
            projection->projected_exprs.push_back(projection->owned_exprs.back().get());
        }

        std::unique_ptr<PhysicalPlanNode> having;
        if (select.having)
        {
            having = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::FILTER);
            having->table_name = "__aggregate__";
            having->over_aggregate = true;
            having->owned_exprs.push_back(rewrite(select.having.get(), true));
            having->predicate = having->owned_exprs.back().get();
        }

        std::unique_ptr<PhysicalPlanNode> sort;
        if (!select.order_by.empty())
        {
            // Positions and aliases name SELECT items; then rewrite
            PhysicalPlanNode resolved(PhysicalPlanType::SORT);
            ResolveSortKeys(select, scope, &resolved);
            sort = std::make_unique<PhysicalPlanNode>(PhysicalPlanType::SORT);
            sort->over_aggregate = true;
            sort->sort_descending = resolved.sort_descending;
            for (const Expression *key : resolved.sort_keys)
            {
                sort->owned_exprs.push_back(rewrite(key, true));
                sort->sort_keys.push_back(sort->owned_exprs.back().get());
            }
        }

        aggregate->children.push_back(std::move(input));
        std::unique_ptr<PhysicalPlanNode> current = std::move(aggregate);
        if (having)
        {
            having->children.push_back(std::move(current));
            current = std::move(having);
        }
        if (sort)
        {
            sort->children.push_back(std::move(current));
            current = std::move(sort);
        }
        projection->children.push_back(std::move(current));
        return projection;
    }

    void Optimizer::ResolveSortKeys(const SelectStatement &select, const Scope &scope, PhysicalPlanNode *sort) const
    {
        for (const auto &order : select.order_by)
        {
            const Expression *key = order.expr.get();

            // ORDER BY 2: the second output column
            if (key->GetType() == ExpressionType::LITERAL)
            {
                const Value &v = static_cast<const LiteralExpression *>(key)->value;
                if (!v.IsNull() && v.GetType() == DataType::INTEGER)
                {
                    const int64_t pos = v.GetAsInt();
                    if (select.select_star)
                    {
                        // Output columns are every table's, in FROM order
                        std::vector<std::string> names;
                        for (const auto &relation : scope.Relations())
                        {
                            for (const auto &col : relation.schema->GetSchema().GetColumns())
                                names.push_back(scope.Size() > 1 ? relation.name + "." + col.name : col.name);
                        }
                        if (pos < 1 || pos > static_cast<int64_t>(names.size()))
                            throw std::runtime_error("ORDER BY position " + std::to_string(pos) + " is out of range");
                        sort->owned_exprs.push_back(std::make_unique<ColumnExpression>(names[static_cast<size_t>(pos - 1)]));
                        key = sort->owned_exprs.back().get();
                    }
                    else
                    {
                        if (pos < 1 || pos > static_cast<int64_t>(select.items.size()))
                            throw std::runtime_error("ORDER BY position " + std::to_string(pos) + " is out of range");
                        key = select.items[static_cast<size_t>(pos - 1)].expr.get();
                    }
                }
            }
            // ORDER BY alias: that output column's expression
            else if (key->GetType() == ExpressionType::COLUMN_REF)
            {
                const std::string &name = static_cast<const ColumnExpression *>(key)->name;
                for (const auto &item : select.items)
                {
                    if (!item.alias.empty() && item.alias == name)
                    {
                        key = item.expr.get();
                        break;
                    }
                }
            }

            // With DISTINCT, rows are only defined by the selected values
            if (select.distinct)
            {
                auto same_column = [&](const std::string &a, const std::string &b)
                { return scope.Canonical(a) == scope.Canonical(b); };
                bool selected = select.select_star && key->GetType() == ExpressionType::COLUMN_REF;
                for (const auto &item : select.items)
                    selected = selected || ExpressionsEqual(item.expr.get(), key, same_column);
                if (!selected)
                    throw std::runtime_error("With SELECT DISTINCT, ORDER BY expressions must appear in the select list");
            }

            sort->sort_keys.push_back(key);
            sort->sort_descending.push_back(order.descending);
        }
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
            out << pad << "NestedLoopJoin(type=" << JoinTypeName(node->join_type) << ", right=" << node->right_label;
            if (!node->join_on_label.empty())
                out << ", on=" << node->join_on_label;
            out << ")";
            break;
        case PhysicalPlanType::HASH_JOIN:
            out << pad << "HashJoin(type=" << JoinTypeName(node->join_type) << ", right=" << node->right_label
                << ", key=" << node->join_key_label << ", build=" << (node->join_build_right ? "right" : "left");
            if (!node->join_residual.empty())
                out << ", on=" << node->join_on_label;
            out << ")";
            break;
        case PhysicalPlanType::INDEX_NESTED_LOOP_JOIN:
            out << pad << "IndexNestedLoopJoin(type=" << JoinTypeName(node->join_type) << ", right="
                << node->right_label << ", key=" << node->join_key_label
                << ", probe=" << (node->join_outer_is_left ? "right" : "left") << " index";
            if (!node->join_residual.empty())
                out << ", on=" << node->join_on_label;
            out << ")";
            break;
        case PhysicalPlanType::FILTER:
            out << pad << "Filter";
            if (node->predicate != nullptr)
            {
                out << "\n" << pad << "  predicate:\n" << DumpExpression(node->predicate, indent + 2);
            }
            break;
        case PhysicalPlanType::SORT:
            out << pad << "Sort(keys=[";
            for (size_t i = 0; i < node->sort_keys.size(); ++i)
            {
                out << (i ? ", " : "") << ExpressionToSQL(node->sort_keys[i]) << (node->sort_descending[i] ? " DESC" : "");
            }
            out << "])";
            break;
        case PhysicalPlanType::DISTINCT:
            out << pad << "Distinct";
            break;
        case PhysicalPlanType::AGGREGATE:
            out << pad << "HashAggregate(group=[";
            for (size_t i = 0; i < node->group_keys.size(); ++i)
                out << (i ? ", " : "") << ExpressionToSQL(node->group_keys[i]);
            out << "], aggregates=[";
            for (size_t i = 0; i < node->aggregates.size(); ++i)
                out << (i ? ", " : "") << ExpressionToSQL(node->aggregates[i]);
            out << "])";
            break;
        case PhysicalPlanType::LIMIT:
            out << pad << "Limit(" << (node->limit ? "limit=" + std::to_string(*node->limit) : std::string("no limit"))
                << ", offset=" << node->offset << ")";
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
