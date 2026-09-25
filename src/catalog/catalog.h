#pragma once

#include "storage/table.h"
#include "storage/btree.h"
#include "storage/buffer_pool.h"
#include "storage/pager.h"
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>

namespace sql
{

    class Catalog
    {
    public:
        static constexpr size_t DEFAULT_POOL_SIZE = 256;

        // Transient database held entirely in memory
        Catalog();

        // Database stored through the given pager / buffer pool. Loads every
        // table and index recorded in the schema table (creating the schema
        // table on a fresh database). Throws std::runtime_error if the stored
        // schema is corrupt.
        Catalog(BufferPoolManager *bpm, Pager *pager);

        Catalog(const Catalog &) = delete;
        Catalog &operator=(const Catalog &) = delete;

        BufferPoolManager *GetBufferPool() const { return bpm_; }

        // Names starting with this prefix are reserved for internal objects
        static constexpr const char *RESERVED_PREFIX = "__";

        // Create a new table in the catalog. Returns false if it already
        // exists; throws std::invalid_argument for a reserved name.
        bool CreateTable(const std::string &name, const Schema &schema);

        // Drop a table from the catalog
        bool DropTable(const std::string &name);

        // Get a table by name (returns nullptr if not found)
        Table *GetTable(const std::string &name);

        // Check if a table exists
        bool TableExists(const std::string &name) const;

        // Get all table names, sorted
        std::vector<std::string> GetTableNames() const;

        // Build a BTree index on table.column from current data; returns false on error
        bool CreateIndex(const std::string &index_name, const std::string &table_name,
                         const std::string &column_name);

        // Retrieve the BTree for a given table/column (nullptr if no index)
        BTree *GetIndex(const std::string &table_name, const std::string &column_name);

        // Rebuild all indexes for a table (call after INSERT / DELETE / UPDATE)
        void RebuildIndexesForTable(const std::string &table_name);

    private:
        // Owned storage for in-memory catalogs. Declared before tables_ so
        // tables are destroyed first.
        std::unique_ptr<Pager> owned_pager_;
        std::unique_ptr<BufferPoolManager> owned_bpm_;
        BufferPoolManager *bpm_ = nullptr;
        Pager *pager_ = nullptr;

        // Schema table (like sqlite_master): one row per table / index,
        // (type, name, tbl_name, root_page, definition). Its first page is
        // the pager's catalog root.
        std::unique_ptr<TableHeap> schema_heap_;
        // "table:<name>" / "index:<name>" -> location of its schema row
        std::unordered_map<std::string, RID> schema_rows_;

        void LoadSchema();
        RID InsertSchemaRow(const std::string &type, const std::string &name,
                            const std::string &tbl_name, page_id_t root_page,
                            const std::string &definition);
        void DeleteSchemaRow(const std::string &type, const std::string &name);

        // Build (or rebuild) the BTree for table.column from current rows
        void BuildIndex(Table *table, int col_idx, BTree *btree);

        std::unordered_map<std::string, std::unique_ptr<Table>> tables_;

        // table_name -> (column_name -> BTree)
        std::unordered_map<std::string, std::unordered_map<std::string, BTree>> indexes_;

        // index_name -> (table_name, column_name) — for duplicate detection
        std::unordered_map<std::string, std::pair<std::string, std::string>> index_registry_;
    };

} // namespace sql
