#include "catalog/catalog.h"

namespace sql
{

    Catalog::Catalog()
        : owned_pager_(std::make_unique<Pager>())
    {
        owned_pager_->OpenInMemory();
        owned_bpm_ = std::make_unique<BufferPoolManager>(DEFAULT_POOL_SIZE, owned_pager_.get());
        bpm_ = owned_bpm_.get();
    }

    Catalog::Catalog(BufferPoolManager *bpm) : bpm_(bpm) {}

    bool Catalog::CreateTable(const std::string &name, const Schema &schema)
    {
        if (TableExists(name))
        {
            return false; // Table already exists
        }
        tables_[name] = std::make_unique<Table>(name, schema, TableHeap::Create(bpm_));
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

        // Indexes on the table point into freed pages; remove them too
        indexes_.erase(name);
        for (auto reg = index_registry_.begin(); reg != index_registry_.end();)
        {
            if (reg->second.first == name)
                reg = index_registry_.erase(reg);
            else
                ++reg;
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
        BTree &btree = indexes_[table_name][column_name];
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
        btree.BulkLoad(entries);

        index_registry_[index_name] = {table_name, column_name};
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
            btree.BulkLoad(entries);
        }
    }

} // namespace sql
