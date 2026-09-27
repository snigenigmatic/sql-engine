#include "cli/shell.h"
#include "parser/ast.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace sql
{

    namespace
    {
        std::string Lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string Trim(const std::string &text)
        {
            const size_t start = text.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                return "";
            return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
        }

        bool IsNumber(const Value &value)
        {
            return !value.IsNull() && (value.GetType() == DataType::INTEGER || value.GetType() == DataType::FLOAT);
        }

        std::string Pad(const std::string &text, size_t width, bool right)
        {
            const std::string fill(width > text.size() ? width - text.size() : 0, ' ');
            return right ? fill + text : text + fill;
        }
    } // namespace

    std::string FormatResult(const ExecutionResult &result)
    {
        if (!result.success)
            return "Error: " + result.message + "\n";
        if (result.tuples.empty() && !result.message.empty())
            return result.message + "\n"; // DDL / DML

        const size_t columns = result.column_names.size();
        std::vector<size_t> width(columns);
        std::vector<bool> right(columns, false);
        for (size_t c = 0; c < columns; ++c)
            width[c] = result.column_names[c].size();
        for (const auto &row : result.tuples)
        {
            for (size_t c = 0; c < columns && c < row.GetValueCount(); ++c)
            {
                width[c] = std::max(width[c], row.GetValue(c).ToString().size());
                right[c] = right[c] || IsNumber(row.GetValue(c));
            }
        }

        std::string out;
        for (size_t c = 0; c < columns; ++c)
            out += (c ? " | " : "") + Pad(result.column_names[c], width[c], right[c]);
        out += "\n";
        for (size_t c = 0; c < columns; ++c)
            out += (c ? "-+-" : "") + std::string(width[c], '-');
        out += "\n";
        for (const auto &row : result.tuples)
        {
            for (size_t c = 0; c < columns && c < row.GetValueCount(); ++c)
                out += (c ? " | " : "") + Pad(row.GetValue(c).ToString(), width[c], right[c]);
            // No trailing spaces after a left-aligned last column
            out.erase(out.find_last_not_of(' ') + 1);
            out += "\n";
        }
        const size_t n = result.tuples.size();
        out += "(" + std::to_string(n) + (n == 1 ? " row)\n" : " rows)\n");
        return out;
    }

    std::string SchemaSQL(Catalog &catalog, const std::string &table_name)
    {
        Table *table = catalog.GetTable(table_name);
        if (table == nullptr)
            return "";
        std::string sql = "CREATE TABLE " + table_name + " (";
        const auto &columns = table->GetSchema().GetColumns();
        for (size_t i = 0; i < columns.size(); ++i)
        {
            const Column &col = columns[i];
            sql += (i ? ", " : "") + col.name + " " + DataTypeName(col.type);
            if (col.type == DataType::VARCHAR && col.length > 0)
                sql += "(" + std::to_string(col.length) + ")";
            if (col.primary_key)
                sql += " PRIMARY KEY";
            else
            {
                if (col.not_null)
                    sql += " NOT NULL";
                if (col.unique)
                    sql += " UNIQUE";
            }
            if (col.default_value)
            {
                const LiteralExpression literal(*col.default_value);
                sql += " DEFAULT " + ExpressionToSQL(&literal);
            }
        }
        sql += ");\n";
        for (const IndexInfo *index : catalog.GetTableIndexes(table_name))
        {
            if (index->name.rfind("__", 0) == 0)
                continue; // behind PRIMARY KEY / UNIQUE
            sql += std::string(index->unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ") + index->name + " ON " +
                   table_name + " (" + index->column + ");\n";
        }
        return sql;
    }

    std::string Shell::Prompt() const
    {
        if (!buffer_.empty())
            return "  -> ";
        return session_->InTransaction() ? "sql*> " : "sql> ";
    }

    void Shell::PrintHelp()
    {
        out_ << "Commands:\n"
             << "  .help              Show this message\n"
             << "  .tables            List tables with their columns and row counts\n"
             << "  .schema [TABLE]    Show CREATE statements for one table, or all\n"
             << "  .read FILE         Run the SQL in FILE (stops at the first error)\n"
             << "  .timer on|off      Show how long each statement takes\n"
             << "  .save              Checkpoint: copy the write-ahead log into the database file\n"
             << "  .quit              Exit (an open transaction is rolled back)\n"
             << "\nSQL statements end with ';' and may span lines, e.g.:\n"
             << "  CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(50) NOT NULL);\n"
             << "  INSERT INTO t VALUES (1, 'hello');\n"
             << "  SELECT name, COUNT(*) FROM t GROUP BY name ORDER BY 2 DESC LIMIT 10;\n"
             << "  BEGIN; ... COMMIT;   (or ROLLBACK;)\n"
             << "\nDatabase: " << db_->GetPath() << " (write-ahead log: " << db_->GetPath() << "-wal)\n";
    }

    void Shell::ListTables()
    {
        auto names = db_->GetCatalog().GetTableNames();
        std::sort(names.begin(), names.end());
        if (names.empty())
        {
            out_ << "No tables.\n";
            return;
        }
        for (const auto &name : names)
        {
            Table *table = db_->GetCatalog().GetTable(name);
            out_ << name << " (" << table->GetTupleCount() << " rows): ";
            const auto &columns = table->GetSchema().GetColumns();
            for (size_t i = 0; i < columns.size(); ++i)
                out_ << (i ? ", " : "") << columns[i].name << " " << DataTypeName(columns[i].type);
            out_ << "\n";
        }
    }

    void Shell::RunSQL(const std::string &sql)
    {
        const auto start = std::chrono::steady_clock::now();
        try
        {
            for (const auto &result : session_->ExecuteScript(sql))
                out_ << FormatResult(result);
        }
        catch (const std::exception &e)
        {
            out_ << "Error: " << e.what() << "\n";
        }
        if (timer_)
        {
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            char text[32];
            std::snprintf(text, sizeof(text), "%.3f", ms);
            out_ << "Time: " << text << " ms\n";
        }
    }

    bool Shell::ReadFile(const std::string &path)
    {
        std::ifstream file(path);
        if (!file)
        {
            out_ << "Error: cannot open " << path << "\n";
            return false;
        }
        std::stringstream text;
        text << file.rdbuf();
        std::vector<ExecutionResult> results;
        try
        {
            results = session_->ExecuteScript(text.str(), true);
        }
        catch (const std::exception &e)
        {
            out_ << "Error: " << e.what() << "\n";
            return false;
        }
        for (const auto &result : results)
            out_ << FormatResult(result);
        if (!results.empty() && !results.back().success)
        {
            out_ << "Stopped " << path << " after statement " << results.size() << ".\n";
            return false;
        }
        return true;
    }

    bool Shell::HandleCommand(const std::string &line)
    {
        std::istringstream words(line);
        std::string command, argument;
        words >> command;
        std::getline(words, argument);
        argument = Trim(argument);
        command = Lower(command);
        if (!command.empty() && command[0] == '.')
            command = command.substr(1);

        if (command == "quit" || command == "exit" || command == "q")
            return false;
        if (command == "help" || command == "h")
            PrintHelp();
        else if (command == "tables")
            ListTables();
        else if (command == "schema")
        {
            auto names = db_->GetCatalog().GetTableNames();
            std::sort(names.begin(), names.end());
            if (!argument.empty())
            {
                if (db_->GetCatalog().GetTable(argument) == nullptr)
                    out_ << "Error: no table named " << argument << "\n";
                names = {argument};
            }
            for (const auto &name : names)
                out_ << SchemaSQL(db_->GetCatalog(), name);
        }
        else if (command == "read")
        {
            if (argument.empty())
                out_ << "Usage: .read FILE\n";
            else
                ReadFile(argument);
        }
        else if (command == "timer")
        {
            const std::string setting = Lower(argument);
            if (setting == "on" || setting == "off")
                timer_ = setting == "on";
            else
                out_ << "Usage: .timer on|off\n";
        }
        else if (command == "save")
        {
            std::string error;
            if (session_->Checkpoint(&error))
                out_ << "Checkpoint complete: all changes are in " << db_->GetPath() << ".\n";
            else
                out_ << "Error: " << error << "\n";
        }
        else
            out_ << "Unknown command: " << line << " (try .help)\n";
        return true;
    }

    bool Shell::HandleLine(const std::string &raw)
    {
        const std::string line = Trim(raw);
        if (line.empty())
            return true;
        if (buffer_.empty())
        {
            // Dot commands, and the older bare words
            const std::string word = Lower(line.substr(0, line.find(' ')));
            if (line[0] == '.' || word == "help" || word == "h" || word == "quit" || word == "exit" ||
                word == "q" || word == "save" || word == "tables")
                return HandleCommand(line);
        }
        buffer_ += (buffer_.empty() ? "" : "\n") + line;
        if (buffer_.back() == ';')
        {
            const std::string sql = buffer_;
            buffer_.clear();
            RunSQL(sql);
        }
        return true;
    }

} // namespace sql
