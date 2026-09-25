#pragma once

#include <string>
#include "catalog/catalog.h"

namespace sql
{

    // Reads the text snapshots written by earlier versions of the REPL
    // (".sqlengine/catalog.meta" + one ".tbl" file per table) into a catalog.
    // Used once to migrate old data into a database file.
    class LegacyImporter
    {
    public:
        explicit LegacyImporter(const std::string &db_directory = ".sqlengine");

        // True if a legacy snapshot exists in the directory
        bool HasSnapshot() const;

        // Import every table in the snapshot. Returns false if there is none.
        bool LoadCatalog(Catalog &catalog);

        const std::string &GetDirectory() const { return db_directory_; }

    private:
        bool LoadTable(const std::string &table_name, Catalog &catalog);
        std::string GetTablePath(const std::string &table_name) const;
        std::string GetCatalogPath() const;

        std::string db_directory_;
    };

} // namespace sql
