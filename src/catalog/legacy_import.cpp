#include "catalog/legacy_import.h"
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <iostream>

namespace sql
{

    LegacyImporter::LegacyImporter(const std::string &db_directory) : db_directory_(db_directory) {}

    bool LegacyImporter::HasSnapshot() const
    {
        struct stat st;
        return stat(GetCatalogPath().c_str(), &st) == 0;
    }

    std::string LegacyImporter::GetTablePath(const std::string &table_name) const
    {
        return db_directory_ + "/" + table_name + ".tbl";
    }

    std::string LegacyImporter::GetCatalogPath() const
    {
        return db_directory_ + "/catalog.meta";
    }

    bool LegacyImporter::LoadCatalog(Catalog &catalog)
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

    bool LegacyImporter::LoadTable(const std::string &table_name, Catalog &catalog)
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
