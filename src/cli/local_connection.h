#pragma once

#include "catalog/database.h"
#include "session/connection.h"
#include "session/session.h"

namespace sql
{

    // A Connection to a database open in this process
    class LocalConnection : public Connection
    {
    public:
        LocalConnection(Database *db, Session *session) : db_(db), session_(session) {}

        ExecutionResult Execute(const std::string &sql) override { return session_->Execute(sql); }
        std::vector<ExecutionResult> ExecuteScript(const std::string &sql, bool stop_on_error = false) override
        {
            return session_->ExecuteScript(sql, stop_on_error);
        }
        bool InTransaction() override { return session_->InTransaction(); }
        std::string Command(const std::string &name, const std::string &argument) override;

    private:
        std::string ListTables();

        Database *db_;
        Session *session_;
    };

} // namespace sql
