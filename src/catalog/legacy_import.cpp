#include "catalog/legacy_import.h"
#include "catalog/database.h"
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace sql
{

    namespace
    {
        template <typename T>
        void ReadOrThrow(std::istream &in, T *value, const std::string &what, const std::string &file)
        {
            if (!(in >> *value))
                throw std::runtime_error("Malformed legacy snapshot " + file + ": expected " + what);
        }

        // Make a rename durable by syncing the containing directory
        void SyncParentDirectory(const std::string &path)
        {
            const size_t slash = path.find_last_of('/');
            const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
            int fd = open(dir.c_str(), O_RDONLY);
            if (fd >= 0)
            {
                fsync(fd);
                close(fd);
            }
        }
    } // namespace

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
        const std::string meta_path = GetCatalogPath();
        std::ifstream meta_file(meta_path);
        if (!meta_file)
        {
            return false; // No saved catalog
        }

        size_t num_tables;
        ReadOrThrow(meta_file, &num_tables, "table count", meta_path);

        for (size_t i = 0; i < num_tables; ++i)
        {
            std::string table_name;
            ReadOrThrow(meta_file, &table_name, "table name", meta_path);
            LoadTable(table_name, catalog);
        }

        return true;
    }

    void LegacyImporter::LoadTable(const std::string &table_name, Catalog &catalog)
    {
        const std::string path = GetTablePath(table_name);
        std::ifstream file(path);
        if (!file)
        {
            throw std::runtime_error("Legacy snapshot is missing table file " + path);
        }

        // Read schema
        size_t num_columns;
        ReadOrThrow(file, &num_columns, "column count", path);
        if (num_columns == 0)
            throw std::runtime_error("Malformed legacy snapshot " + path + ": table has no columns");

        std::vector<Column> columns;
        for (size_t i = 0; i < num_columns; ++i)
        {
            std::string name;
            int type_int, length;
            ReadOrThrow(file, &name, "column name", path);
            ReadOrThrow(file, &type_int, "column type", path);
            ReadOrThrow(file, &length, "column length", path);
            if (type_int < 0 || type_int > static_cast<int>(DataType::BOOLEAN))
                throw std::runtime_error("Malformed legacy snapshot " + path + ": unknown column type");
            columns.emplace_back(name, static_cast<DataType>(type_int), length);
        }

        Schema schema(columns);
        if (!catalog.CreateTable(table_name, schema))
            throw std::runtime_error("Legacy snapshot lists table '" + table_name + "' twice");
        Table *table = catalog.GetTable(table_name);

        // Read tuples
        size_t num_tuples;
        ReadOrThrow(file, &num_tuples, "row count", path);

        for (size_t t = 0; t < num_tuples; ++t)
        {
            std::vector<Value> values;
            for (size_t c = 0; c < num_columns; ++c)
            {
                int type_int;
                ReadOrThrow(file, &type_int, "value type", path);
                if (type_int < 0 || type_int > static_cast<int>(DataType::BOOLEAN))
                    throw std::runtime_error("Malformed legacy snapshot " + path + ": unknown value type");
                DataType type = static_cast<DataType>(type_int);

                std::string val_str;
                ReadOrThrow(file, &val_str, "value", path);

                if (val_str == "NULL")
                {
                    values.emplace_back(type);
                    continue;
                }

                switch (type)
                {
                case DataType::INTEGER:
                    values.emplace_back(static_cast<int32_t>(std::stoi(val_str)));
                    break;
                case DataType::FLOAT:
                    values.emplace_back(std::stod(val_str));
                    break;
                case DataType::BOOLEAN:
                    if (val_str != "true" && val_str != "false")
                        throw std::runtime_error("Malformed legacy snapshot " + path + ": bad boolean '" + val_str + "'");
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
                        // Content containing spaces spans several tokens
                        while (content.length() < len)
                        {
                            std::string more;
                            ReadOrThrow(file, &more, "rest of string value", path);
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
            table->Insert(Tuple(values));
        }
    }

    bool MigrateLegacySnapshot(const std::string &snapshot_dir, const std::string &db_path,
                               size_t *tables_imported, std::string *error)
    {
        const std::string tmp_path = db_path + ".importing";
        std::remove(tmp_path.c_str()); // leftover from an interrupted attempt

        auto fail = [&](const std::string &message)
        {
            if (error)
                *error = message;
            std::remove(tmp_path.c_str());
            return false;
        };

        {
            std::string open_error;
            auto db = Database::Open(tmp_path, &open_error);
            if (!db)
                return fail(open_error);

            try
            {
                LegacyImporter importer(snapshot_dir);
                if (!importer.LoadCatalog(db->GetCatalog()))
                {
                    db.reset();
                    return fail("no legacy snapshot in " + snapshot_dir);
                }
            }
            catch (const std::exception &e)
            {
                db.reset();
                return fail(e.what());
            }

            if (!db->Flush())
            {
                db.reset();
                return fail("failed to write " + tmp_path);
            }
            if (tables_imported)
                *tables_imported = db->GetCatalog().GetTableNames().size();
        } // closes the file

        if (std::rename(tmp_path.c_str(), db_path.c_str()) != 0)
            return fail("failed to move " + tmp_path + " to " + db_path);
        SyncParentDirectory(db_path);
        return true;
    }

} // namespace sql
