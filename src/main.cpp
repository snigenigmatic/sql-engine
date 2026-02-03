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

// Global catalog and disk manager for the REPL
sql::Catalog g_catalog;
sql::DiskManager g_disk_manager;

void PrintBanner()
{
    std::cout << "========================================\n";
    std::cout << "  SQL Engine v0.1.0\n";
    std::cout << "  Type 'help' for help, 'quit' to exit\n";
    std::cout << "========================================\n\n";
}

void PrintHelp()
{
    std::cout << "Available commands:\n";
    std::cout << "  help   - Show this help message\n";
    std::cout << "  quit   - Exit the program (auto-saves)\n";
    std::cout << "  save   - Save all tables to disk\n";
    std::cout << "  test   - Run a simple test\n";
    std::cout << "  tables - List all tables\n";
    std::cout << "  demo   - Create demo table with sample data\n";
    std::cout << "\nSQL commands (end with semicolon):\n";
    std::cout << "  SELECT * FROM table_name;\n";
    std::cout << "  SELECT col1, col2 FROM table_name WHERE condition;\n";
    std::cout << "\nData is stored in: .sqlengine/\n";
}

void PrintResults(const sql::ExecutionResult &result)
{
    if (!result.success)
    {
        std::cout << "Error: " << result.error_message << "\n";
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

    // Print separator
    for (size_t i = 0; i < result.column_names.size(); ++i)
    {
        if (i > 0)
            std::cout << "-+-";
        std::cout << std::string(12, '-');
    }
    std::cout << "\n";

    // Print rows
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

void CreateDemoTable()
{
    using namespace sql;

    // Create schema for "users" table
    std::vector<Column> columns = {
        Column("id", DataType::INTEGER),
        Column("name", DataType::VARCHAR, 50),
        Column("age", DataType::INTEGER),
        Column("active", DataType::BOOLEAN)};
    Schema schema(columns);

    // Create table
    if (!g_catalog.CreateTable("users", schema))
    {
        std::cout << "Table 'users' already exists.\n";
        return;
    }

    // Insert sample data
    Table *table = g_catalog.GetTable("users");
    table->Insert(Tuple({Value(1), Value("Alice"), Value(25), Value(true)}));
    table->Insert(Tuple({Value(2), Value("Bob"), Value(30), Value(true)}));
    table->Insert(Tuple({Value(3), Value("Charlie"), Value(35), Value(false)}));
    table->Insert(Tuple({Value(4), Value("Diana"), Value(28), Value(true)}));
    table->Insert(Tuple({Value(5), Value("Eve"), Value(22), Value(false)}));

    std::cout << "Created table 'users' with 5 rows.\n";
    std::cout << "Try: SELECT * FROM users;\n";
    std::cout << "     SELECT name, age FROM users WHERE age > 25;\n\n";
}

void ListTables()
{
    auto names = g_catalog.GetTableNames();
    if (names.empty())
    {
        std::cout << "No tables. Type 'demo' to create a sample table.\n";
        return;
    }
    std::cout << "Tables:\n";
    for (const auto &name : names)
    {
        sql::Table *t = g_catalog.GetTable(name);
        std::cout << "  " << name << " (" << t->GetTupleCount() << " rows)\n";
    }
}

void RunSimpleTest()
{
    using namespace sql;

    std::cout << "\n=== Running Simple Test ===\n";

    // Create a schema
    std::vector<Column> columns = {
        Column("id", DataType::INTEGER),
        Column("name", DataType::VARCHAR, 50),
        Column("age", DataType::INTEGER)};
    Schema schema(columns);

    std::cout << "Schema created with " << schema.GetColumnCount() << " columns\n";

    // Create a tuple
    std::vector<Value> values = {
        Value(1),
        Value("Alice"),
        Value(25)};
    Tuple tuple(values, &schema);

    std::cout << "Tuple created: " << tuple.ToString() << "\n";

    // Access values
    std::cout << "ID: " << tuple.GetValue(0).GetAsInt() << "\n";
    std::cout << "Name: " << tuple.GetValue(1).GetAsString() << "\n";
    std::cout << "Age: " << tuple.GetValue(2).GetAsInt() << "\n";

    // Test comparisons
    Value v1(10);
    Value v2(20);
    std::cout << "\nComparison test: 10 < 20 = "
              << (v1 < v2 ? "true" : "false") << "\n";

    std::cout << "=== Test Complete ===\n\n";
}

void ExecuteSQL(const std::string &sql)
{
    try
    {
        // Tokenize and Parse
        sql::Lexer lexer(sql);
        sql::Parser parser(lexer);
        auto stmt = parser.ParseStatement();

        if (!stmt)
        {
            std::cout << "Error: Failed to parse SQL statement.\n";
            return;
        }

        // Execute
        sql::Executor executor(&g_catalog);
        auto result = executor.Execute(stmt.get());

        // Print results
        PrintResults(result);
    }
    catch (const std::exception &e)
    {
        std::cout << "Error: " << e.what() << "\n";
    }
}

int main()
{
    // Load saved data from disk
    if (g_disk_manager.LoadCatalog(g_catalog))
    {
        auto tables = g_catalog.GetTableNames();
        if (!tables.empty())
        {
            std::cout << "Loaded " << tables.size() << " table(s) from disk.\n";
        }
    }

    PrintBanner();

    std::string input;
    std::string sql_buffer;

    while (true)
    {
        if (sql_buffer.empty())
        {
            std::cout << "sql> ";
        }
        else
        {
            std::cout << "  -> ";
        }

        std::getline(std::cin, input);

        if (std::cin.eof())
        {
            std::cout << "\nGoodbye!\n";
            break;
        }

        // Trim whitespace
        size_t start = input.find_first_not_of(" \t\n\r");
        size_t end = input.find_last_not_of(" \t\n\r");

        if (start == std::string::npos)
        {
            continue; // Empty line
        }

        input = input.substr(start, end - start + 1);

        // Check for simple commands (when not in multi-line SQL mode)
        if (sql_buffer.empty())
        {
            std::string command = input;
            for (char &c : command)
            {
                c = std::tolower(c);
            }

            if (command == "quit" || command == "exit" || command == "q")
            {
                // Auto-save on exit
                if (g_disk_manager.SaveCatalog(g_catalog))
                {
                    std::cout << "Data saved.\n";
                }
                std::cout << "Goodbye!\n";
                break;
            }
            else if (command == "save")
            {
                if (g_disk_manager.SaveCatalog(g_catalog))
                {
                    std::cout << "All tables saved to disk.\n";
                }
                else
                {
                    std::cout << "Error saving data.\n";
                }
                continue;
            }
            else if (command == "help" || command == "h")
            {
                PrintHelp();
                continue;
            }
            else if (command == "test")
            {
                RunSimpleTest();
                continue;
            }
            else if (command == "demo")
            {
                CreateDemoTable();
                continue;
            }
            else if (command == "tables")
            {
                ListTables();
                continue;
            }
        }

        // Accumulate SQL input
        sql_buffer += " " + input;

        // Check if statement is complete (ends with semicolon)
        if (sql_buffer.back() == ';')
        {
            // Keep the semicolon for parsing (parser expects it)
            ExecuteSQL(sql_buffer);
            sql_buffer.clear();
        }
    }

    return 0;
}