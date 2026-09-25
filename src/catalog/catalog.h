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

    // A named index on one column of a table, backed by an on-disk B+ tree
    struct IndexInfo
    {
        std::string name;
        std::string table;
        std::string column;
        int column_index = -1;
        bool unique = false; // no two rows may share a non-NULL key
        std::unique_ptr<BTree> tree;
    };

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

        // Create a new table in the catalog, with a unique index for each
        // PRIMARY KEY / UNIQUE column. Returns false if it already exists;
        // throws std::invalid_argument for a reserved name.
        bool CreateTable(const std::string &name, const Schema &schema);

        // Drop a table from the catalog
        bool DropTable(const std::string &name);

        // Get a table by name (returns nullptr if not found)
        Table *GetTable(const std::string &name);

        // Check if a table exists
        bool TableExists(const std::string &name) const;

        // Get all table names, sorted
        std::vector<std::string> GetTableNames() const;

        // Build a BTree index on table.column from current data. Returns false
        // if the name is taken or the table/column does not exist; throws
        // std::invalid_argument for a reserved name, a value that cannot be
        // indexed, or (for a unique index) duplicate values.
        bool CreateIndex(const std::string &index_name, const std::string &table_name,
                         const std::string &column_name, bool unique = false);

        // Retrieve a BTree for a given table/column (nullptr if no index)
        BTree *GetIndex(const std::string &table_name, const std::string &column_name);

        // All indexes defined on a table, ordered by index name
        std::vector<IndexInfo *> GetTableIndexes(const std::string &table_name);

        // Row modifications that keep every index on the table in sync.
        // Index keys and uniqueness are validated before the table is
        // touched, so a row is either written with all its index entries or
        // not at all. A duplicate key throws std::runtime_error
        // ("UNIQUE constraint failed: table.column").
        RID InsertRow(Table *table, const Tuple &tuple);
        bool DeleteRow(Table *table, const RID &rid);
        bool UpdateRow(Table *table, const RID &rid, const Tuple &tuple);

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

        // Create a tree for the index and insert every existing row
        void PopulateIndex(Table *table, IndexInfo *index);
        void CheckIndexKeys(const std::vector<IndexInfo *> &indexes, const Tuple &tuple) const;
        // Throws if a unique index already holds one of the tuple's keys for
        // a row other than `self`
        void CheckUnique(const std::vector<IndexInfo *> &indexes, const Tuple &tuple, const RID *self) const;
        bool CreateIndexInternal(const std::string &index_name, const std::string &table_name,
                                 const std::string &column_name, bool unique);

        std::unordered_map<std::string, std::unique_ptr<Table>> tables_;

        // index name -> index
        std::unordered_map<std::string, std::unique_ptr<IndexInfo>> indexes_;
    };

} // namespace sql
