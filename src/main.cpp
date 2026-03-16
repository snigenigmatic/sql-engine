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
#include "storage/disk_manager.h"

sql::Catalog g_catalog;
sql::DiskManager g_disk_manager;

void PrintBanner()
{
    std::cout << "========================================\n";
    std::cout << "  SQL Engine v0.2.0\n";
    std::cout << "  Type 'help' for help, 'quit' to exit\n";
    std::cout << "========================================\n\n";
}

void PrintHelp()
{
    std::cout << "Available commands:\n";
    std::cout << "  help   - Show this help message\n";
    std::cout << "  quit   - Exit the program (auto-saves)\n";
    std::cout << "  save   - Save all tables to disk\n";
    std::cout << "  tables - List all tables\n";
    std::cout << "\nSQL commands (end with semicolon):\n";
    std::cout << "  CREATE TABLE t (col1 INTEGER, col2 VARCHAR(50), col3 BOOLEAN);\n";
    std::cout << "  INSERT INTO t VALUES (1, 'hello', TRUE);\n";
    std::cout << "  SELECT * FROM t;\n";
    std::cout << "  SELECT col1, col2 FROM t WHERE col1 > 5;\n";
    std::cout << "  UPDATE t SET col1 = 10 WHERE col2 = 'hello';\n";
    std::cout << "  DELETE FROM t WHERE col1 = 1;\n";
    std::cout << "\nData is stored in: .sqlengine/\n";
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
    auto names = g_catalog.GetTableNames();
    if (names.empty())
    {
        std::cout << "No tables.\n";
        return;
    }
    std::cout << "Tables:\n";
    for (const auto &name : names)
    {
        sql::Table *t = g_catalog.GetTable(name);
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

        sql::Executor executor(&g_catalog);
        auto result = executor.Execute(stmt.get());
        PrintResults(result);
    }
    catch (const std::exception &e)
    {
        std::cout << "Error: " << e.what() << "\n";
    }
}

int main()
{
    if (g_disk_manager.LoadCatalog(g_catalog))
    {
        auto tables = g_catalog.GetTableNames();
        if (!tables.empty())
            std::cout << "Loaded " << tables.size() << " table(s) from disk.\n";
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
                if (g_disk_manager.SaveCatalog(g_catalog))
                    std::cout << "Data saved.\n";
                std::cout << "Goodbye!\n";
                break;
            }
            else if (command == "save")
            {
                if (g_disk_manager.SaveCatalog(g_catalog))
                    std::cout << "All tables saved to disk.\n";
                else
                    std::cout << "Error saving data.\n";
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

    return 0;
}
