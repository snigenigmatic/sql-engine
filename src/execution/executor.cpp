#include "execution/executor.h"
#include <stdexcept>

namespace sql
{

    ExecutionResult Executor::Execute(Statement *stmt)
    {
        if (!stmt)
        {
            return {false, "Null statement", {}, {}};
        }

        switch (stmt->GetType())
        {
        case StatementType::SELECT:
            return ExecuteSelect(static_cast<SelectStatement *>(stmt));
        default:
            return {false, "Unsupported statement type", {}, {}};
        }
    }

    std::unique_ptr<Operator> Executor::BuildPlan(SelectStatement *select)
    {
        // Get the table from catalog
        Table *table = catalog_->GetTable(select->table);
        if (!table)
        {
            throw std::runtime_error("Table not found: " + select->table);
        }

        // Step 1: SeqScan (read all tuples from table)
        std::unique_ptr<Operator> plan = std::make_unique<SeqScan>(table);

        // Step 2: Filter (if WHERE clause exists)
        if (select->where)
        {
            plan = std::make_unique<Filter>(std::move(plan), select->where.get(), table);
        }

        // Step 3: Projection (select specific columns or *)
        if (select->select_star)
        {
            plan = std::make_unique<Projection>(std::move(plan), std::vector<int>{}, true);
        }
        else
        {
            std::vector<int> column_indices;
            for (const auto &col_name : select->columns)
            {
                int idx = table->GetColumnIndex(col_name);
                if (idx < 0)
                {
                    throw std::runtime_error("Unknown column: " + col_name);
                }
                column_indices.push_back(idx);
            }
            plan = std::make_unique<Projection>(std::move(plan), column_indices, false);
        }

        return plan;
    }

    ExecutionResult Executor::ExecuteSelect(SelectStatement *select)
    {
        ExecutionResult result;

        try
        {
            // Build the execution plan
            auto plan = BuildPlan(select);

            // Determine column names for output
            Table *table = catalog_->GetTable(select->table);
            if (select->select_star)
            {
                const auto &cols = table->GetSchema().GetColumns();
                for (const auto &col : cols)
                {
                    result.column_names.push_back(col.name);
                }
            }
            else
            {
                result.column_names = select->columns;
            }

            // Execute the plan using Volcano model
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
            result.error_message = e.what();
        }

        return result;
    }

} // namespace sql
