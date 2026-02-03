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

} // namespace sql
