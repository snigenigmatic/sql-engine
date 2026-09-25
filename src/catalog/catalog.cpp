#include "catalog/catalog.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace sql
{

    namespace
    {
        constexpr const char *TYPE_TABLE = "table";
        constexpr const char *TYPE_INDEX = "index";

        // Schema definition text, one entry per column separated by ',':
        //   name:type:length                         (written by older versions)
        //   name:type:length:flags:default_hex
        // flags: 1 = NOT NULL, 2 = PRIMARY KEY, 4 = UNIQUE. default_hex is
        // the default value's binary encoding in hex (empty = no default),
        // which keeps arbitrary strings clear of the separators.
        constexpr int FLAG_NOT_NULL = 1;
        constexpr int FLAG_PRIMARY_KEY = 2;
        constexpr int FLAG_UNIQUE = 4;

        std::string ToHex(const std::string &bytes)
        {
            static const char *digits = "0123456789abcdef";
            std::string out;
            for (unsigned char c : bytes)
            {
                out += digits[c >> 4];
                out += digits[c & 15];
            }
            return out;
        }

        std::string FromHex(const std::string &hex)
        {
            if (hex.size() % 2 != 0)
                throw std::runtime_error("Corrupt default value in schema");
            std::string out;
            for (size_t i = 0; i < hex.size(); i += 2)
                out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
            return out;
        }

        std::string EncodeSchema(const Schema &schema)
        {
            std::string out;
            for (const auto &col : schema.GetColumns())
            {
                if (!out.empty())
                    out += ",";
                const int flags = (col.not_null ? FLAG_NOT_NULL : 0) | (col.primary_key ? FLAG_PRIMARY_KEY : 0) |
                                  (col.unique ? FLAG_UNIQUE : 0);
                std::string default_bytes;
                if (col.default_value)
                    col.default_value->SerializeTo(&default_bytes);
                out += col.name + ":" + std::to_string(static_cast<int>(col.type)) + ":" +
                       std::to_string(col.length) + ":" + std::to_string(flags) + ":" + ToHex(default_bytes);
            }
            return out;
        }

        Schema DecodeSchema(const std::string &text)
        {
            std::vector<Column> columns;
            std::stringstream ss(text);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                std::vector<std::string> fields;
                std::stringstream fs(item);
                std::string field;
                while (std::getline(fs, field, ':'))
                    fields.push_back(field);
                if (item.back() == ':')
                    fields.push_back(""); // empty default
                if ((fields.size() != 3 && fields.size() != 5) || fields[0].empty())
                    throw std::runtime_error("Corrupt table schema: " + text);

                const int type = std::stoi(fields[1]);
                if (type < 0 || type > static_cast<int>(DataType::BOOLEAN))
                    throw std::runtime_error("Corrupt column type in schema: " + text);
                Column column(fields[0], static_cast<DataType>(type), std::stoi(fields[2]));
                if (fields.size() == 5)
                {
                    const int flags = std::stoi(fields[3]);
                    column.not_null = (flags & FLAG_NOT_NULL) != 0;
                    column.primary_key = (flags & FLAG_PRIMARY_KEY) != 0;
                    column.unique = (flags & FLAG_UNIQUE) != 0;
                    if (!fields[4].empty())
                    {
                        const std::string bytes = FromHex(fields[4]);
                        const char *cursor = bytes.data();
                        Value value;
                        if (!Value::DeserializeFrom(&cursor, bytes.data() + bytes.size(), &value))
                            throw std::runtime_error("Corrupt default value in schema: " + text);
                        column.default_value = value;
                    }
                }
                columns.push_back(std::move(column));
            }
            if (columns.empty())
                throw std::runtime_error("Corrupt table schema: " + text);
            return Schema(std::move(columns));
        }

        // Index definition text: the column, plus ":unique" for a unique index
        std::string EncodeIndexDefinition(const std::string &column, bool unique)
        {
            return unique ? column + ":unique" : column;
        }

        void DecodeIndexDefinition(const std::string &text, std::string *column, bool *unique)
        {
            const size_t colon = text.find(':');
            *column = text.substr(0, colon);
            *unique = colon != std::string::npos && text.substr(colon + 1) == "unique";
        }

        std::string SchemaKey(const std::string &type, const std::string &name)
        {
            return type + ":" + name;
        }
    } // namespace

    Catalog::Catalog()
        : owned_pager_(std::make_unique<Pager>())
    {
        owned_pager_->OpenInMemory();
        owned_bpm_ = std::make_unique<BufferPoolManager>(DEFAULT_POOL_SIZE, owned_pager_.get());
        bpm_ = owned_bpm_.get();
        pager_ = owned_pager_.get();
        LoadSchema();
    }

    Catalog::Catalog(BufferPoolManager *bpm, Pager *pager) : bpm_(bpm), pager_(pager)
    {
        LoadSchema();
    }

    void Catalog::LoadSchema()
    {
        const page_id_t root = pager_->GetCatalogRoot();
        if (root == INVALID_PAGE_ID)
        {
            // Only a brand-new file (just the header page) may lack a schema;
            // anything else is corrupt or an interrupted initialization, and
            // creating a new schema would orphan its pages.
            if (pager_->GetPageCount() != 1)
                throw std::runtime_error("Database has " + std::to_string(pager_->GetPageCount()) +
                                         " pages but no schema table");
            schema_heap_ = TableHeap::Create(bpm_);
            // Make the schema page durable before the header points at it
            if (!bpm_->FlushPage(schema_heap_->GetFirstPageId()) ||
                !pager_->SetCatalogRoot(schema_heap_->GetFirstPageId()))
                throw std::runtime_error("Unable to initialize schema table");
            return;
        }

        schema_heap_ = std::make_unique<TableHeap>(bpm_, root);

        struct IndexDef
        {
            std::string name, table, column;
            page_id_t root_page;
        };
        std::vector<IndexDef> index_defs;

        RID rid;
        Tuple row;
        for (bool ok = schema_heap_->FirstTuple(&rid, &row); ok; ok = schema_heap_->NextTuple(rid, &rid, &row))
        {
            if (row.GetValueCount() != 5)
                throw std::runtime_error("Corrupt schema row at " + rid.ToString());
            const std::string type = row.GetValue(0).GetAsString();
            const std::string name = row.GetValue(1).GetAsString();
            const std::string tbl_name = row.GetValue(2).GetAsString();
            const page_id_t root_page = row.GetValue(3).GetAsInt();
            const std::string definition = row.GetValue(4).GetAsString();

            if (type == TYPE_TABLE)
            {
                if (root_page < 1 || static_cast<uint32_t>(root_page) >= pager_->GetPageCount())
                    throw std::runtime_error("Table '" + name + "' has invalid root page " + std::to_string(root_page));
                tables_[name] = std::make_unique<Table>(
                    name, DecodeSchema(definition), std::make_unique<TableHeap>(bpm_, root_page));
            }
            else if (type == TYPE_INDEX)
            {
                // INVALID_PAGE_ID marks an index written before indexes were
                // stored on disk; it is rebuilt below
                if (root_page != INVALID_PAGE_ID &&
                    (root_page < 1 || static_cast<uint32_t>(root_page) >= pager_->GetPageCount()))
                    throw std::runtime_error("Index '" + name + "' has invalid root page " + std::to_string(root_page));
                index_defs.push_back({name, tbl_name, definition, root_page});
            }
            else
            {
                throw std::runtime_error("Unknown schema object type: " + type);
            }
            schema_rows_[SchemaKey(type, name)] = rid;
        }

        for (const auto &def : index_defs)
        {
            std::string column;
            bool unique = false;
            DecodeIndexDefinition(def.column, &column, &unique);
            Table *table = GetTable(def.table);
            const int col_idx = table ? table->GetColumnIndex(column) : -1;
            if (col_idx < 0)
                throw std::runtime_error("Index " + def.name + " refers to missing " + def.table + "." + column);

            auto index = std::make_unique<IndexInfo>();
            index->name = def.name;
            index->table = def.table;
            index->column = column;
            index->column_index = col_idx;
            index->unique = unique;
            if (def.root_page != INVALID_PAGE_ID)
            {
                index->tree = std::make_unique<BTree>(bpm_, def.root_page);
            }
            else
            {
                // Written before indexes were stored on disk: build it now
                // and record its root
                PopulateIndex(table, index.get());
                DeleteSchemaRow(TYPE_INDEX, def.name);
                InsertSchemaRow(TYPE_INDEX, def.name, def.table, index->tree->GetRootPageId(),
                                EncodeIndexDefinition(column, unique));
            }
            indexes_[def.name] = std::move(index);
        }
    }

    RID Catalog::InsertSchemaRow(const std::string &type, const std::string &name,
                                 const std::string &tbl_name, page_id_t root_page,
                                 const std::string &definition)
    {
        RID rid = schema_heap_->InsertTuple(Tuple({Value(type), Value(name), Value(tbl_name),
                                                   Value(static_cast<int32_t>(root_page)), Value(definition)}));
        schema_rows_[SchemaKey(type, name)] = rid;
        return rid;
    }

    void Catalog::DeleteSchemaRow(const std::string &type, const std::string &name)
    {
        auto it = schema_rows_.find(SchemaKey(type, name));
        if (it == schema_rows_.end())
            return;
        schema_heap_->DeleteTuple(it->second);
        schema_rows_.erase(it);
    }

    void Catalog::PopulateIndex(Table *table, IndexInfo *index)
    {
        index->tree = BTree::Create(bpm_);
        try
        {
            for (auto it = table->begin(); it != table->end(); ++it)
            {
                const Value &value = it->GetValue(static_cast<size_t>(index->column_index));
                if (value.IsNull()) // NULLs are not indexed
                    continue;
                if (index->unique && !index->tree->Search(value).empty())
                    throw std::invalid_argument("Cannot create unique index '" + index->name + "': " +
                                                index->table + "." + index->column + " has duplicate value " +
                                                value.ToString());
                index->tree->Insert(value, it.GetRID());
            }
        }
        catch (...)
        {
            index->tree->Drop();
            index->tree.reset();
            throw;
        }
    }

    void Catalog::CheckIndexKeys(const std::vector<IndexInfo *> &indexes, const Tuple &tuple) const
    {
        for (const IndexInfo *index : indexes)
        {
            const Value &value = tuple.GetValue(static_cast<size_t>(index->column_index));
            if (!value.IsNull())
                BTree::CheckKey(value);
        }
    }

    void Catalog::CheckUnique(const std::vector<IndexInfo *> &indexes, const Tuple &tuple, const RID *self) const
    {
        for (const IndexInfo *index : indexes)
        {
            if (!index->unique)
                continue;
            const Value &value = tuple.GetValue(static_cast<size_t>(index->column_index));
            if (value.IsNull()) // any number of NULLs is allowed
                continue;
            for (const RID &existing : index->tree->Search(value))
            {
                if (self == nullptr || existing != *self)
                    throw std::runtime_error("UNIQUE constraint failed: " + index->table + "." + index->column);
            }
        }
    }

    std::vector<IndexInfo *> Catalog::GetTableIndexes(const std::string &table_name)
    {
        std::vector<IndexInfo *> result;
        for (auto &entry : indexes_)
        {
            if (entry.second->table == table_name)
                result.push_back(entry.second.get());
        }
        // Deterministic order (by name) so row operations are reproducible
        std::sort(result.begin(), result.end(), [](const IndexInfo *a, const IndexInfo *b)
                  { return a->name < b->name; });
        return result;
    }

    namespace
    {
        // An index entry touched by a row operation, kept so the operation
        // can be undone if a later step fails
        struct IndexEntry
        {
            BTree *tree;
            Value key;
        };

        // Undo helpers are best effort: storage that just failed may fail
        // again, and the original error is the one worth reporting
        void InsertEntries(const std::vector<IndexEntry> &entries, const RID &rid) noexcept
        {
            for (const auto &entry : entries)
            {
                try
                {
                    entry.tree->Insert(entry.key, rid);
                }
                catch (...)
                {
                }
            }
        }

        void RemoveEntries(const std::vector<IndexEntry> &entries, const RID &rid) noexcept
        {
            for (const auto &entry : entries)
            {
                try
                {
                    entry.tree->Remove(entry.key, rid);
                }
                catch (...)
                {
                }
            }
        }
    } // namespace

    RID Catalog::InsertRow(Table *table, const Tuple &tuple)
    {
        auto indexes = GetTableIndexes(table->GetName());
        CheckIndexKeys(indexes, tuple);
        CheckUnique(indexes, tuple, nullptr);
        RID rid = table->Insert(tuple);

        std::vector<IndexEntry> inserted;
        try
        {
            for (IndexInfo *index : indexes)
            {
                const Value &value = tuple.GetValue(static_cast<size_t>(index->column_index));
                if (value.IsNull())
                    continue;
                index->tree->Insert(value, rid);
                inserted.push_back({index->tree.get(), value});
            }
        }
        catch (...)
        {
            RemoveEntries(inserted, rid);
            try
            {
                table->DeleteTuple(rid);
            }
            catch (...)
            {
            }
            throw;
        }
        return rid;
    }

    bool Catalog::DeleteRow(Table *table, const RID &rid)
    {
        Tuple old_tuple;
        if (!table->GetTuple(rid, &old_tuple))
            return false;

        // Remove index entries first so a failure never leaves entries
        // pointing at a deleted row
        std::vector<IndexEntry> removed;
        try
        {
            for (IndexInfo *index : GetTableIndexes(table->GetName()))
            {
                const Value &value = old_tuple.GetValue(static_cast<size_t>(index->column_index));
                if (!value.IsNull() && index->tree->Remove(value, rid))
                    removed.push_back({index->tree.get(), value});
            }
            if (!table->DeleteTuple(rid))
            {
                InsertEntries(removed, rid);
                return false;
            }
        }
        catch (...)
        {
            InsertEntries(removed, rid);
            throw;
        }
        return true;
    }

    bool Catalog::UpdateRow(Table *table, const RID &rid, const Tuple &tuple)
    {
        Tuple old_tuple;
        if (!table->GetTuple(rid, &old_tuple))
            return false;
        auto indexes = GetTableIndexes(table->GetName());
        CheckIndexKeys(indexes, tuple);
        CheckUnique(indexes, tuple, &rid);

        // 1. Remove the old entries, then 2. rewrite the row (it may move)
        std::vector<IndexEntry> removed;
        RID new_rid;
        try
        {
            for (IndexInfo *index : indexes)
            {
                const Value &old_value = old_tuple.GetValue(static_cast<size_t>(index->column_index));
                if (!old_value.IsNull() && index->tree->Remove(old_value, rid))
                    removed.push_back({index->tree.get(), old_value});
            }
            if (!table->UpdateTuple(rid, tuple, &new_rid))
            {
                InsertEntries(removed, rid);
                return false;
            }
        }
        catch (...)
        {
            InsertEntries(removed, rid);
            throw;
        }

        // 3. Add the new entries at the row's final location
        std::vector<IndexEntry> new_entries;
        for (IndexInfo *index : indexes)
        {
            const Value &new_value = tuple.GetValue(static_cast<size_t>(index->column_index));
            if (!new_value.IsNull())
                new_entries.push_back({index->tree.get(), new_value});
        }
        size_t inserted = 0;
        try
        {
            for (; inserted < new_entries.size(); ++inserted)
                new_entries[inserted].tree->Insert(new_entries[inserted].key, new_rid);
        }
        catch (...)
        {
            RemoveEntries(std::vector<IndexEntry>(new_entries.begin(), new_entries.begin() + static_cast<long>(inserted)),
                          new_rid);
            // Put the old contents back (the row may move again) and point
            // the old entries at it. If that fails too, index the row as it
            // is actually stored.
            RID restored = new_rid;
            bool reverted = false;
            try
            {
                reverted = table->UpdateTuple(new_rid, old_tuple, &restored);
            }
            catch (...)
            {
            }
            if (reverted)
                InsertEntries(removed, restored);
            else
                InsertEntries(new_entries, new_rid);
            throw;
        }
        return true;
    }

    bool Catalog::CreateTable(const std::string &name, const Schema &schema)
    {
        if (name.rfind(RESERVED_PREFIX, 0) == 0)
        {
            throw std::invalid_argument("Table name '" + name + "' is reserved (names may not start with '" +
                                        RESERVED_PREFIX + "')");
        }
        if (TableExists(name))
        {
            return false; // Table already exists
        }
        auto heap = TableHeap::Create(bpm_);
        const page_id_t root = heap->GetFirstPageId();
        tables_[name] = std::make_unique<Table>(name, schema, std::move(heap));
        InsertSchemaRow(TYPE_TABLE, name, name, root, EncodeSchema(schema));

        // PRIMARY KEY and UNIQUE are enforced (and served) by unique indexes
        for (const auto &col : schema.GetColumns())
        {
            if (col.primary_key)
                CreateIndexInternal(std::string(RESERVED_PREFIX) + "pk_" + name, name, col.name, true);
            else if (col.unique)
                CreateIndexInternal(std::string(RESERVED_PREFIX) + "uq_" + name + "_" + col.name, name, col.name, true);
        }
        return true;
    }

    bool Catalog::DropTable(const std::string &name)
    {
        auto it = tables_.find(name);
        if (it == tables_.end())
        {
            return false; // Table doesn't exist
        }
        it->second->Drop();
        tables_.erase(it);
        DeleteSchemaRow(TYPE_TABLE, name);

        // Drop the table's indexes and free their pages
        for (auto idx = indexes_.begin(); idx != indexes_.end();)
        {
            if (idx->second->table == name)
            {
                idx->second->tree->Drop();
                DeleteSchemaRow(TYPE_INDEX, idx->first);
                idx = indexes_.erase(idx);
            }
            else
            {
                ++idx;
            }
        }
        return true;
    }

    Table *Catalog::GetTable(const std::string &name)
    {
        auto it = tables_.find(name);
        if (it == tables_.end())
        {
            return nullptr;
        }
        return it->second.get();
    }

    bool Catalog::TableExists(const std::string &name) const
    {
        return tables_.find(name) != tables_.end();
    }

    std::vector<std::string> Catalog::GetTableNames() const
    {
        std::vector<std::string> names;
        names.reserve(tables_.size());
        for (const auto &pair : tables_)
        {
            names.push_back(pair.first);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    bool Catalog::CreateIndex(const std::string &index_name, const std::string &table_name,
                              const std::string &column_name, bool unique)
    {
        if (index_name.rfind(RESERVED_PREFIX, 0) == 0)
        {
            throw std::invalid_argument("Index name '" + index_name + "' is reserved (names may not start with '" +
                                        RESERVED_PREFIX + "')");
        }
        return CreateIndexInternal(index_name, table_name, column_name, unique);
    }

    bool Catalog::CreateIndexInternal(const std::string &index_name, const std::string &table_name,
                                      const std::string &column_name, bool unique)
    {
        if (indexes_.count(index_name))
            return false; // index name already taken

        Table *table = GetTable(table_name);
        if (!table)
            return false;

        int col_idx = table->GetColumnIndex(column_name);
        if (col_idx < 0)
            return false;

        auto index = std::make_unique<IndexInfo>();
        index->name = index_name;
        index->table = table_name;
        index->column = column_name;
        index->column_index = col_idx;
        index->unique = unique;
        PopulateIndex(table, index.get());

        InsertSchemaRow(TYPE_INDEX, index_name, table_name, index->tree->GetRootPageId(),
                        EncodeIndexDefinition(column_name, unique));
        indexes_[index_name] = std::move(index);
        return true;
    }

    BTree *Catalog::GetIndex(const std::string &table_name, const std::string &column_name)
    {
        for (auto &entry : indexes_)
        {
            if (entry.second->table == table_name && entry.second->column == column_name)
                return entry.second->tree.get();
        }
        return nullptr;
    }

} // namespace sql
