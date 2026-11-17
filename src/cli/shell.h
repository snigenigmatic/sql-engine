#pragma once

#include "catalog/catalog.h"
#include "execution/executor.h"
#include "session/connection.h"
#include <ostream>
#include <string>

namespace sql
{

    // A result as the REPL prints it: an error, a status message, or a table
    // whose columns are as wide as their widest value (numbers right-aligned,
    // text left-aligned) followed by the row count
    std::string FormatResult(const ExecutionResult &result);

    // CREATE TABLE statement for a table, then one CREATE INDEX per index
    // the user made (not those behind PRIMARY KEY / UNIQUE)
    std::string SchemaSQL(Catalog &catalog, const std::string &table);

    // The interactive shell: collects lines into statements, runs them over
    // a Connection (local or remote), and handles dot commands (.help,
    // .tables, .schema, .read, .timer, .save, .quit). Output goes to `out`.
    class Shell
    {
    public:
        Shell(Connection *connection, std::ostream &out) : connection_(connection), out_(out) {}

        // Handle one line of input; false once the user asked to quit
        bool HandleLine(const std::string &line);

        // "sql> ", "sql*> " inside a transaction, "  -> " mid-statement
        std::string Prompt();

        // Run a file of SQL, stopping at the first failing statement.
        // Returns false if the file cannot be read or a statement failed.
        bool ReadFile(const std::string &path);

        void PrintHelp();

    private:
        bool HandleCommand(const std::string &command);
        void RunSQL(const std::string &sql);
        // A question for the database (.tables, .schema, .save)
        void RunCommand(const std::string &name, const std::string &argument);
        void PrintNotice(); // the connection's notice, if any

        Connection *connection_;
        std::ostream &out_;
        std::string buffer_; // an unfinished statement
        bool timer_ = false;
    };

} // namespace sql
