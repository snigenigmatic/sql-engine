#include "cli/local_connection.h"
#include "cli/shell.h"
#include <algorithm>

namespace sql
{

    std::string LocalConnection::ListTables()
    {
        auto names = db_->GetCatalog().GetTableNames();
        std::sort(names.begin(), names.end());
        if (names.empty())
            return "No tables.\n";
        std::string out;
        for (const auto &name : names)
        {
            Table *table = db_->GetCatalog().GetTable(name);
            out += name + " (" + std::to_string(table->GetTupleCount()) + " rows): ";
            const auto &columns = table->GetSchema().GetColumns();
            for (size_t i = 0; i < columns.size(); ++i)
                out += (i ? ", " : "") + columns[i].name + " " + DataTypeName(columns[i].type);
            if (db_->GetCatalog().GetStats(name) != nullptr)
                out += " (analyzed)";
            out += "\n";
        }
        return out;
    }

    std::string LocalConnection::Command(const std::string &name, const std::string &argument)
    {
        if (name == "tables")
            return ListTables();
        if (name == "schema")
        {
            auto names = db_->GetCatalog().GetTableNames();
            std::sort(names.begin(), names.end());
            std::string out;
            if (!argument.empty())
            {
                if (db_->GetCatalog().GetTable(argument) == nullptr)
                    out += "Error: no table named " + argument + "\n";
                names = {argument};
            }
            for (const auto &table : names)
                out += SchemaSQL(db_->GetCatalog(), table);
            return out;
        }
        if (name == "save")
        {
            std::string error;
            if (session_->Checkpoint(&error))
                return "Checkpoint complete: all changes are in " + db_->GetPath() + ".\n";
            return "Error: " + error + "\n";
        }
        if (name == "describe")
            return "Database: " + db_->GetPath() + " (write-ahead log: " + db_->GetPath() + "-wal)\n";
        if (name == "cluster")
            return "Error: .cluster needs a connection to a cluster (sqlengine --connect to its coordinators)\n";
        return "Error: unknown command " + name + "\n";
    }

} // namespace sql
