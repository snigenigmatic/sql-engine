#include "storage/disk_manager.h"
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <iostream>

namespace sql
{

    DiskManager::DiskManager(const std::string &db_directory) : db_directory_(db_directory)
    {
        EnsureDirectory();
    }

    bool DiskManager::EnsureDirectory()
    {
        struct stat st;
        if (stat(db_directory_.c_str(), &st) != 0)
        {
            // Directory doesn't exist, create it
            return mkdir(db_directory_.c_str(), 0755) == 0;
        }
        return true;
    }

    std::string DiskManager::GetTablePath(const std::string &table_name) const
    {
        return db_directory_ + "/" + table_name + ".tbl";
    }

    std::string DiskManager::GetCatalogPath() const
    {
        return db_directory_ + "/catalog.meta";
    }

    bool DiskManager::SaveCatalog(const Catalog &catalog)
    {
        // Save catalog metadata (list of table names)
        std::ofstream meta_file(GetCatalogPath());
        if (!meta_file)
        {
            return false;
        }

        auto table_names = catalog.GetTableNames();
        meta_file << table_names.size() << "\n";
        for (const auto &name : table_names)
        {
            meta_file << name << "\n";
            // Save each table
            Table *table = const_cast<Catalog &>(catalog).GetTable(name);
            if (table && !SaveTable(*table))
            {
                return false;
            }
        }

        return true;
    }

    bool DiskManager::SaveTable(const Table &table)
    {
        std::ofstream file(GetTablePath(table.GetName()));
        if (!file)
        {
            return false;
        }

        const Schema &schema = table.GetSchema();
        const auto &columns = schema.GetColumns();

        // Write schema: num_columns
        file << columns.size() << "\n";

        // Write each column: name type length
        for (const auto &col : columns)
        {
            file << col.name << " " << static_cast<int>(col.type) << " " << col.length << "\n";
        }

        // Write tuples: num_tuples
        const auto &tuples = table.GetTuples();
        file << tuples.size() << "\n";

        // Write each tuple
        for (const auto &tuple : tuples)
        {
            for (size_t i = 0; i < columns.size(); ++i)
            {
                const Value &val = tuple.GetValue(i);
                file << static_cast<int>(val.GetType()) << " ";
                if (val.IsNull())
                {
                    file << "NULL";
                }
                else
                {
                    switch (val.GetType())
                    {
                    case DataType::INTEGER:
                        file << val.GetAsInt();
                        break;
                    case DataType::FLOAT:
                        file << val.GetAsFloat();
                        break;
                    case DataType::BOOLEAN:
                        file << (val.GetAsBool() ? "true" : "false");
                        break;
                    case DataType::VARCHAR:
                    {
                        // Escape string: write length then content
                        std::string s = val.GetAsString();
                        file << s.length() << ":" << s;
                        break;
                    }
                    }
                }
                file << "\n";
            }
        }

        return true;
    }

    bool DiskManager::LoadCatalog(Catalog &catalog)
    {
        std::ifstream meta_file(GetCatalogPath());
        if (!meta_file)
        {
            return false; // No saved catalog, that's OK
        }

        size_t num_tables;
        meta_file >> num_tables;

        for (size_t i = 0; i < num_tables; ++i)
        {
            std::string table_name;
            meta_file >> table_name;
            if (!LoadTable(table_name, catalog))
            {
                std::cerr << "Warning: Failed to load table " << table_name << "\n";
            }
        }

        return true;
    }

    bool DiskManager::LoadTable(const std::string &table_name, Catalog &catalog)
    {
        std::ifstream file(GetTablePath(table_name));
        if (!file)
        {
            return false;
        }

        // Read schema
        size_t num_columns;
        file >> num_columns;

        std::vector<Column> columns;
        for (size_t i = 0; i < num_columns; ++i)
        {
            std::string name;
            int type_int, length;
            file >> name >> type_int >> length;
            columns.emplace_back(name, static_cast<DataType>(type_int), length);
        }

        Schema schema(columns);
        catalog.CreateTable(table_name, schema);
        Table *table = catalog.GetTable(table_name);

        // Read tuples
        size_t num_tuples;
        file >> num_tuples;

        for (size_t t = 0; t < num_tuples; ++t)
        {
            std::vector<Value> values;
            for (size_t c = 0; c < num_columns; ++c)
            {
                int type_int;
                file >> type_int;
                DataType type = static_cast<DataType>(type_int);

                std::string val_str;
                file >> val_str;

                if (val_str == "NULL")
                {
                    values.emplace_back(type);
                }
                else
                {
                    switch (type)
                    {
                    case DataType::INTEGER:
                        values.emplace_back(std::stoi(val_str));
                        break;
                    case DataType::FLOAT:
                        values.emplace_back(std::stod(val_str));
                        break;
                    case DataType::BOOLEAN:
                        values.emplace_back(val_str == "true");
                        break;
                    case DataType::VARCHAR:
                    {
                        // Parse length:content format
                        size_t colon_pos = val_str.find(':');
                        if (colon_pos != std::string::npos)
                        {
                            size_t len = std::stoul(val_str.substr(0, colon_pos));
                            std::string content = val_str.substr(colon_pos + 1);
                            // Read remaining characters if needed
                            while (content.length() < len)
                            {
                                std::string more;
                                file >> more;
                                content += " " + more;
                            }
                            values.emplace_back(content.substr(0, len));
                        }
                        else
                        {
                            values.emplace_back(val_str);
                        }
                        break;
                    }
                    }
                }
            }
            table->Insert(Tuple(values));
        }

        return true;
    }

} // namespace sql
