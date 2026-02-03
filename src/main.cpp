#include <iostream>
#include <string>
#include "common/value.h"
#include "common/schema.h"
#include "common/tuple.h"

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
    std::cout << "  help  - Show this help message\n";
    std::cout << "  quit  - Exit the program\n";
    std::cout << "  test  - Run a simple test\n";
    std::cout << "\nSQL support coming soon!\n";
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

int main()
{
    PrintBanner();

    std::string input;

    while (true)
    {
        std::cout << "sql> ";
        std::getline(std::cin, input);

        // Trim whitespace
        size_t start = input.find_first_not_of(" \t\n\r");
        size_t end = input.find_last_not_of(" \t\n\r");

        if (start == std::string::npos)
        {
            continue; // Empty line
        }

        input = input.substr(start, end - start + 1);

        // Convert to lowercase for command matching
        std::string command = input;
        for (char &c : command)
        {
            c = std::tolower(c);
        }

        if (command == "quit" || command == "exit" || command == "q")
        {
            std::cout << "Goodbye!\n";
            break;
        }
        else if (command == "help" || command == "h")
        {
            PrintHelp();
        }
        else if (command == "test")
        {
            RunSimpleTest();
        }
        else
        {
            std::cout << "Command not recognized. ";
            std::cout << "SQL parsing not yet implemented.\n";
            std::cout << "Type 'help' for available commands.\n";
        }
    }

    return 0;
}