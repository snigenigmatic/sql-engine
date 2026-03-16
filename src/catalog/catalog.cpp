#include "catalog/catalog.h"

namespace sql
{

    bool Catalog::CreateTable(const std::string &name, const Schema &schema)
    {
        if (TableExists(name))
        {
            return false; // Table already exists
        }
        tables_[name] = std::make_unique<Table>(name, schema);
        return true;
    }

    bool Catalog::DropTable(const std::string &name)
    {
        auto it = tables_.find(name);
        if (it == tables_.end())
        {
            return false; // Table doesn't exist
        }
        tables_.erase(it);
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
        const auto &tuples = table->GetTuples();
        std::vector<std::pair<Value, size_t>> entries;
        entries.reserve(tuples.size());
        for (size_t i = 0; i < tuples.size(); ++i)
            entries.push_back({tuples[i].GetValue(static_cast<size_t>(col_idx)), i});
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

        const auto &tuples = table->GetTuples();
        for (auto &[col_name, btree] : t_it->second)
        {
            int col_idx = table->GetColumnIndex(col_name);
            if (col_idx < 0)
                continue;

            std::vector<std::pair<Value, size_t>> entries;
            entries.reserve(tuples.size());
            for (size_t i = 0; i < tuples.size(); ++i)
                entries.push_back({tuples[i].GetValue(static_cast<size_t>(col_idx)), i});
            btree.BulkLoad(entries);
        }
    }

} // namespace sql
