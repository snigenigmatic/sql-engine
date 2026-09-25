#pragma once

#include "catalog/database.h"
#include "execution/executor.h"
#include "parser/ast.h"
#include <string>
#include <vector>

namespace sql
{

    // Runs SQL against a Database with transaction semantics.
    //
    // Outside a transaction every statement is its own transaction: it is
    // committed when it succeeds and rolled back when it fails. BEGIN opens
    // an explicit transaction that lasts until COMMIT or ROLLBACK; inside it
    // a failed statement is rolled back on its own (to a savepoint taken
    // before it ran) and the transaction stays open. A transaction still open
    // when the session ends is rolled back.
    //
    // A database file allows one connection at a time, so transactions are
    // trivially serializable.
    class Session
    {
    public:
        explicit Session(Database *db) : db_(db) {}
        ~Session() { Close(); }

        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        // Parse and run exactly one statement. Input with more than one
        // statement is rejected without running any of it.
        ExecutionResult Execute(const std::string &sql);
        ExecutionResult Execute(Statement *stmt);

        // Run every statement in the input, in order, returning one result
        // each. Stops after a statement that cannot be parsed.
        std::vector<ExecutionResult> ExecuteScript(const std::string &sql);

        bool InTransaction() const { return in_transaction_; }

        // Copy the write-ahead log into the database file. Refused inside a
        // transaction, since it would have to commit it.
        bool Checkpoint(std::string *error);

        // Roll back an open transaction
        void Close();

    private:
        ExecutionResult ExecuteTransaction(const TransactionStatement *stmt);

        Database *db_;
        bool in_transaction_ = false;
    };

} // namespace sql
