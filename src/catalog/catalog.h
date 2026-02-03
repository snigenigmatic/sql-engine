#pragma once

#include "storage/table.h"
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>

namespace sql
{

    class Catalog
    {
    public:
        Catalog() = default;

        // Create a new table in the catalog
        bool CreateTable(const std::string &name, const Schema &schema);

        // Drop a table from the catalog
        bool DropTable(const std::string &name);

        // Get a table by name (returns nullptr if not found)
        Table *GetTable(const std::string &name);

        // Check if a table exists
        bool TableExists(const std::string &name) const;

        // Get all table names
        std::vector<std::string> GetTableNames() const;

    private:
        std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
    };

} // namespace sql
