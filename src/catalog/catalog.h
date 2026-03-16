#pragma once

#include "storage/table.h"
#include "storage/btree.h"
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

        // Build a BTree index on table.column from current data; returns false on error
        bool CreateIndex(const std::string &index_name, const std::string &table_name,
                         const std::string &column_name);

        // Retrieve the BTree for a given table/column (nullptr if no index)
        BTree *GetIndex(const std::string &table_name, const std::string &column_name);

        // Rebuild all indexes for a table (call after INSERT / DELETE / UPDATE)
        void RebuildIndexesForTable(const std::string &table_name);

    private:
        std::unordered_map<std::string, std::unique_ptr<Table>> tables_;

        // table_name -> (column_name -> BTree)
        std::unordered_map<std::string, std::unordered_map<std::string, BTree>> indexes_;

        // index_name -> (table_name, column_name) — for duplicate detection
        std::unordered_map<std::string, std::pair<std::string, std::string>> index_registry_;
    };

} // namespace sql
