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

        // Schema definition text: "name:type:length,name:type:length,..."
        std::string EncodeSchema(const Schema &schema)
        {
            std::string out;
            for (const auto &col : schema.GetColumns())
            {
                if (!out.empty())
                    out += ",";
                out += col.name + ":" + std::to_string(static_cast<int>(col.type)) + ":" +
                       std::to_string(col.length);
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
                const size_t a = item.find(':');
                const size_t b = item.find(':', a == std::string::npos ? a : a + 1);
                if (a == std::string::npos || b == std::string::npos || a == 0)
                    throw std::runtime_error("Corrupt table schema: " + text);
                const int type = std::stoi(item.substr(a + 1, b - a - 1));
                if (type < 0 || type > static_cast<int>(DataType::BOOLEAN))
                    throw std::runtime_error("Corrupt column type in schema: " + text);
                columns.emplace_back(item.substr(0, a), static_cast<DataType>(type),
                                     std::stoi(item.substr(b + 1)));
            }
            if (columns.empty())
                throw std::runtime_error("Corrupt table schema: " + text);
            return Schema(std::move(columns));
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
                index_defs.push_back({name, tbl_name, definition});
            }
            else
            {
                throw std::runtime_error("Unknown schema object type: " + type);
            }
            schema_rows_[SchemaKey(type, name)] = rid;
        }

        // Indexes are in-memory for now: rebuild them from table data
        for (const auto &def : index_defs)
        {
            Table *table = GetTable(def.table);
            const int col_idx = table ? table->GetColumnIndex(def.column) : -1;
            if (col_idx < 0)
                throw std::runtime_error("Index " + def.name + " refers to missing " + def.table + "." + def.column);
            BuildIndex(table, col_idx, &indexes_[def.table][def.column]);
            index_registry_[def.name] = {def.table, def.column};
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

    void Catalog::BuildIndex(Table *table, int col_idx, BTree *btree)
    {
        std::vector<std::pair<Value, RID>> entries;
        entries.reserve(table->GetTupleCount());
        for (auto it = table->begin(); it != table->end(); ++it)
        {
            Value value = it->GetValue(static_cast<size_t>(col_idx));
            // Skip NULL values to avoid undefined behavior in sorting/comparison
            if (!value.IsNull())
            {
                entries.push_back({value, it.GetRID()});
            }
        }
        btree->BulkLoad(entries);
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

        // Indexes on the table point into freed pages; remove them too
        indexes_.erase(name);
        for (auto reg = index_registry_.begin(); reg != index_registry_.end();)
        {
            if (reg->second.first == name)
            {
                DeleteSchemaRow(TYPE_INDEX, reg->first);
                reg = index_registry_.erase(reg);
            }
            else
            {
                ++reg;
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
                              const std::string &column_name)
    {
        if (index_registry_.count(index_name))
            return false; // index name already taken

        Table *table = GetTable(table_name);
        if (!table)
            return false;

        int col_idx = table->GetColumnIndex(column_name);
        if (col_idx < 0)
            return false;

        // Build BTree from existing rows
        BuildIndex(table, col_idx, &indexes_[table_name][column_name]);

        index_registry_[index_name] = {table_name, column_name};
        InsertSchemaRow(TYPE_INDEX, index_name, table_name, INVALID_PAGE_ID, column_name);
        return true;
    }

    BTree *Catalog::GetIndex(const std::string &table_name, const std::string &column_name)
    {
        auto t_it = indexes_.find(table_name);
        if (t_it == indexes_.end())
            return nullptr;
        auto c_it = t_it->second.find(column_name);
        if (c_it == t_it->second.end())
            return nullptr;
        return &c_it->second;
    }

    void Catalog::RebuildIndexesForTable(const std::string &table_name)
    {
        auto t_it = indexes_.find(table_name);
        if (t_it == indexes_.end())
            return;

        Table *table = GetTable(table_name);
        if (!table)
            return;

        for (auto &[col_name, btree] : t_it->second)
        {
            int col_idx = table->GetColumnIndex(col_name);
            if (col_idx < 0)
                continue;

            BuildIndex(table, col_idx, &btree);
        }
    }

} // namespace sql
