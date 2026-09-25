#include <iostream>
#include <string>
#include <iomanip>
#include "common/value.h"
#include "common/schema.h"
#include "common/tuple.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "catalog/catalog.h"
#include "execution/executor.h"
#include "catalog/database.h"
#include "catalog/legacy_import.h"
#include <memory>
#include <sys/stat.h>

std::unique_ptr<sql::Database> g_db;

void PrintBanner()
{
    std::cout << "========================================\n";
    std::cout << "  SQL Engine v0.2.0\n";
    std::cout << "  Database: " << g_db->GetPath() << "\n";
    std::cout << "  Type 'help' for help, 'quit' to exit\n";
    std::cout << "========================================\n\n";
}

void PrintHelp()
{
    std::cout << "Available commands:\n";
    std::cout << "  help   - Show this help message\n";
    std::cout << "  quit   - Exit the program\n";
    std::cout << "  save   - Checkpoint: copy the write-ahead log into the database file\n";
    std::cout << "  tables - List all tables\n";
    std::cout << "\nSQL commands (end with semicolon):\n";
    std::cout << "  CREATE TABLE t (col1 INTEGER, col2 VARCHAR(50), col3 BOOLEAN);\n";
    std::cout << "  INSERT INTO t VALUES (1, 'hello', TRUE);\n";
    std::cout << "  SELECT * FROM t;\n";
    std::cout << "  SELECT col1, col2 FROM t WHERE col1 > 5;\n";
    std::cout << "  UPDATE t SET col1 = 10 WHERE col2 = 'hello';\n";
    std::cout << "  DELETE FROM t WHERE col1 = 1;\n";
    std::cout << "\nData is stored in: " << g_db->GetPath() << " (write-ahead log: "
              << g_db->GetPath() << "-wal)\n";
    std::cout << "Each statement is atomic: committed when it succeeds, rolled back when it fails.\n";
}

void PrintResults(const sql::ExecutionResult &result)
{
    if (!result.success)
    {
        std::cout << "Error: " << result.message << "\n";
        return;
    }

    // DDL/DML messages (no tuples to display)
    if (result.tuples.empty() && !result.message.empty())
    {
        std::cout << result.message << "\n";
        return;
    }

    if (result.tuples.empty())
    {
        std::cout << "(0 rows)\n";
        return;
    }

    // Print column headers
    std::cout << "\n";
    for (size_t i = 0; i < result.column_names.size(); ++i)
    {
        if (i > 0)
            std::cout << " | ";
        std::cout << std::setw(12) << result.column_names[i];
    }
    std::cout << "\n";

    for (size_t i = 0; i < result.column_names.size(); ++i)
    {
        if (i > 0)
            std::cout << "-+-";
        std::cout << std::string(12, '-');
    }
    std::cout << "\n";

    for (const auto &tuple : result.tuples)
    {
        for (size_t i = 0; i < tuple.GetValueCount(); ++i)
        {
            if (i > 0)
                std::cout << " | ";
            std::cout << std::setw(12) << tuple.GetValue(i).ToString();
        }
        std::cout << "\n";
    }

    std::cout << "(" << result.tuples.size() << " row"
              << (result.tuples.size() == 1 ? "" : "s") << ")\n\n";
}

void ListTables()
{
    auto names = g_db->GetCatalog().GetTableNames();
    if (names.empty())
    {
        std::cout << "No tables.\n";
        return;
    }
    std::cout << "Tables:\n";
    for (const auto &name : names)
    {
        sql::Table *t = g_db->GetCatalog().GetTable(name);
        const auto &cols = t->GetSchema().GetColumns();
        std::cout << "  " << name << " (" << t->GetTupleCount() << " rows) - columns: ";
        for (size_t i = 0; i < cols.size(); ++i)
        {
            if (i > 0) std::cout << ", ";
            std::cout << cols[i].name;
        }
        std::cout << "\n";
    }
}

// Undo a failed statement's partial changes, if it made any
void RollbackStatement()
{
    if (!g_db->HasUncommittedChanges())
        return;
    std::string error;
    if (g_db->Rollback(&error))
        std::cout << "(statement rolled back)\n";
    else
        std::cout << "Warning: " << error << "\n";
}

void ExecuteSQL(const std::string &sql_input)
{
    try
    {
        sql::Lexer lexer(sql_input);
        sql::Parser parser(lexer);
        auto stmt = parser.ParseStatement();

        if (!stmt)
        {
            std::cout << "Error: Failed to parse SQL statement.\n";
            return;
        }

        sql::ExecutionResult result;
        {
            sql::Executor executor(&g_db->GetCatalog());
            result = executor.Execute(stmt.get());
        }
        PrintResults(result);

        // Each statement is its own transaction: all of it or none of it
        if (result.success)
        {
            if (!g_db->Commit())
            {
                std::cout << "Error: failed to commit to " << g_db->GetPath() << "\n";
                RollbackStatement();
            }
        }
        else
        {
            RollbackStatement();
        }
    }
    catch (const std::exception &e)
    {
        std::cout << "Error: " << e.what() << "\n";
        RollbackStatement();
    }
}

// True if the path names an existing, non-empty file
bool DatabaseFileHasContent(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

int main(int argc, char **argv)
{
    std::string path = "sqlengine.db";
    if (argc > 1)
    {
        std::string arg = argv[1];
        if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: " << argv[0] << " [database-file]   (default: sqlengine.db)\n";
            return 0;
        }
        path = arg;
    }

    // One-time migration of text snapshots written by earlier versions. It
    // only runs when the database file does not exist yet, and is all or
    // nothing: a failed import leaves no database behind and is retried on
    // the next launch.
    std::string error;
    sql::LegacyImporter legacy;
    bool imported = false;
    if (!DatabaseFileHasContent(path) && legacy.HasSnapshot())
    {
        size_t table_count = 0;
        if (!sql::MigrateLegacySnapshot(legacy.GetDirectory(), path, &table_count, &error))
        {
            std::cerr << "Error: could not import legacy snapshot from " << legacy.GetDirectory()
                      << "/: " << error << "\n"
                      << "No database was created. Fix or move the snapshot aside, then retry.\n";
            return 1;
        }
        std::cout << "Imported " << table_count << " table(s) from legacy snapshot "
                  << legacy.GetDirectory() << "/\n";
        imported = true;
    }

    g_db = sql::Database::Open(path, &error);
    if (!g_db)
    {
        std::cerr << "Error: " << error << "\n";
        return 1;
    }

    if (!imported)
    {
        auto tables = g_db->GetCatalog().GetTableNames();
        if (!tables.empty())
            std::cout << "Loaded " << tables.size() << " table(s) from " << path << ".\n";
    }

    PrintBanner();

    std::string input;
    std::string sql_buffer;

    while (true)
    {
        if (sql_buffer.empty())
            std::cout << "sql> ";
        else
            std::cout << "  -> ";

        std::getline(std::cin, input);

        if (std::cin.eof())
        {
            std::cout << "\nGoodbye!\n";
            break;
        }

        size_t start = input.find_first_not_of(" \t\n\r");
        size_t end = input.find_last_not_of(" \t\n\r");
        if (start == std::string::npos)
            continue;
        input = input.substr(start, end - start + 1);

        if (sql_buffer.empty())
        {
            std::string command = input;
            for (char &c : command)
                c = static_cast<char>(std::tolower(c));

            if (command == "quit" || command == "exit" || command == "q")
            {
                std::cout << "Goodbye!\n";
                break;
            }
            else if (command == "save")
            {
                if (g_db->Checkpoint())
                    std::cout << "Checkpoint complete: all changes are in " << g_db->GetPath() << ".\n";
                else
                    std::cout << "Error: checkpoint of " << g_db->GetPath() << " failed.\n";
                continue;
            }
            else if (command == "help" || command == "h")
            {
                PrintHelp();
                continue;
            }
            else if (command == "tables")
            {
                ListTables();
                continue;
            }
        }

        sql_buffer += " " + input;

        if (sql_buffer.back() == ';')
        {
            ExecuteSQL(sql_buffer);
            sql_buffer.clear();
        }
    }

    g_db.reset(); // checkpoints and removes the write-ahead log
    return 0;
}
