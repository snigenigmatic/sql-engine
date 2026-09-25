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

        // Import every table in the snapshot. Returns false if there is no
        // snapshot. Throws std::runtime_error if any listed table is missing
        // or malformed, so a partial import is never mistaken for success.
        bool LoadCatalog(Catalog &catalog);

        const std::string &GetDirectory() const { return db_directory_; }

    private:
        void LoadTable(const std::string &table_name, Catalog &catalog);
        std::string GetTablePath(const std::string &table_name) const;
        std::string GetCatalogPath() const;

        std::string db_directory_;
    };

    // Import the legacy snapshot in snapshot_dir into a new database file at
    // db_path, all or nothing: the database is built in "<db_path>.importing"
    // and renamed into place only after every table has been loaded and
    // flushed. On failure nothing is left at db_path, so the import can be
    // retried. Returns false and sets *error on failure.
    bool MigrateLegacySnapshot(const std::string &snapshot_dir, const std::string &db_path,
                               size_t *tables_imported, std::string *error);

} // namespace sql
