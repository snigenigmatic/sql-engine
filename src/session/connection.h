#pragma once

#include "execution/executor.h"
#include <string>
#include <vector>

namespace sql
{

    // What a client talks to: a database in this process (LocalConnection)
    // or one served over the network (RemoteConnection). Methods may throw
    // std::runtime_error when the connection itself fails.
    class Connection
    {
    public:
        virtual ~Connection() = default;

        // Exactly one statement, as Session::Execute
        virtual ExecutionResult Execute(const std::string &sql) = 0;

        // Every statement in the input, as Session::ExecuteScript
        virtual std::vector<ExecutionResult> ExecuteScript(const std::string &sql, bool stop_on_error = false) = 0;

        virtual bool InTransaction() = 0;

        // Questions about the database itself, answered as text to print:
        // "tables", "schema" (argument: a table, or empty for all), "save"
        // (checkpoint) and "describe" (where the database lives)
        virtual std::string Command(const std::string &name, const std::string &argument) = 0;
    };

} // namespace sql
