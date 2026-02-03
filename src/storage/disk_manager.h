#pragma once

#include <string>
#include <fstream>
#include "catalog/catalog.h"

namespace sql
{

    class DiskManager
    {
    public:
        explicit DiskManager(const std::string &db_directory = ".sqlengine");

        // Save the entire catalog to disk
        bool SaveCatalog(const Catalog &catalog);

        // Load the catalog from disk
        bool LoadCatalog(Catalog &catalog);

        // Get the database directory path
        const std::string &GetDirectory() const { return db_directory_; }

    private:
        // Save a single table to a file
        bool SaveTable(const Table &table);

        // Load a single table from a file
        bool LoadTable(const std::string &table_name, Catalog &catalog);

        // Get the path for a table's data file
        std::string GetTablePath(const std::string &table_name) const;

        // Get the path for the catalog metadata file
        std::string GetCatalogPath() const;

        // Ensure the database directory exists
        bool EnsureDirectory();

        std::string db_directory_;
    };

} // namespace sql
