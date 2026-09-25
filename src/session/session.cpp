#include "session/session.h"
#include "lexer/lexer.h"
#include "parser/parser.h"

namespace sql
{

    namespace
    {
        ExecutionResult Error(const std::string &message)
        {
            ExecutionResult result;
            result.success = false;
            result.message = message;
            return result;
        }

        ExecutionResult Ok(const std::string &message)
        {
            ExecutionResult result;
            result.success = true;
            result.message = message;
            return result;
        }

        bool IsReadOnly(StatementType type)
        {
            return type == StatementType::SELECT || type == StatementType::EXPLAIN_STMT;
        }
    } // namespace

    ExecutionResult Session::Execute(const std::string &sql)
    {
        std::unique_ptr<Statement> stmt;
        try
        {
            Lexer lexer(sql);
            Parser parser(lexer);
            stmt = parser.ParseStatement();
        }
        catch (const std::exception &e)
        {
            return Error(std::string("Parse error: ") + e.what());
        }
        if (!stmt)
            return Error("Failed to parse SQL statement.");
        return Execute(stmt.get());
    }

    ExecutionResult Session::Execute(Statement *stmt)
    {
        if (stmt->GetType() == StatementType::TRANSACTION_STMT)
            return ExecuteTransaction(static_cast<TransactionStatement *>(stmt));

        auto run = [&]
        {
            Executor executor(&db_->GetCatalog());
            return executor.Execute(stmt);
        };

        if (IsReadOnly(stmt->GetType()) || db_->IsInMemory())
            return run(); // nothing to commit, or no log to roll back with

        if (in_transaction_)
        {
            // Statement-level atomicity inside the transaction
            Pager::Savepoint savepoint;
            if (!db_->CreateSavepoint(&savepoint))
                return Error("Unable to write to " + db_->GetPath());
            ExecutionResult result = run();
            if (!result.success)
            {
                std::string error;
                if (db_->RollbackTo(savepoint, &error))
                    result.message += " (statement rolled back; transaction still open)";
                else
                    result.message += " (" + error + ")";
            }
            return result;
        }

        // Auto-commit: the statement is its own transaction
        ExecutionResult result = run();
        if (result.success)
        {
            if (db_->Commit())
                return result;
            result.success = false;
            result.message = "Failed to commit to " + db_->GetPath();
        }
        if (db_->HasUncommittedChanges())
        {
            std::string error;
            if (db_->Rollback(&error))
                result.message += " (statement rolled back)";
            else
                result.message += " (" + error + ")";
        }
        return result;
    }

    ExecutionResult Session::ExecuteTransaction(const TransactionStatement *stmt)
    {
        switch (stmt->kind)
        {
        case TransactionStatement::Kind::BEGIN:
            if (in_transaction_)
                return Error("A transaction is already in progress.");
            if (db_->IsInMemory())
                return Error("Transactions are not supported for in-memory databases.");
            // Anything left over from auto-commit mode is committed already;
            // start from a clean slate
            if (!db_->Commit())
                return Error("Failed to commit to " + db_->GetPath());
            in_transaction_ = true;
            return Ok("Transaction started.");

        case TransactionStatement::Kind::COMMIT:
            if (!in_transaction_)
                return Error("No transaction is in progress.");
            in_transaction_ = false;
            if (!db_->Commit())
            {
                std::string error;
                db_->Rollback(&error);
                return Error("Commit failed; the transaction was rolled back.");
            }
            return Ok("Transaction committed.");

        case TransactionStatement::Kind::ROLLBACK:
        {
            if (!in_transaction_)
                return Error("No transaction is in progress.");
            in_transaction_ = false;
            std::string error;
            if (!db_->Rollback(&error))
                return Error(error);
            return Ok("Transaction rolled back.");
        }
        }
        return Error("Unknown transaction statement");
    }

    bool Session::Checkpoint(std::string *error)
    {
        if (in_transaction_)
        {
            if (error)
                *error = "Cannot checkpoint inside a transaction; COMMIT or ROLLBACK first.";
            return false;
        }
        if (!db_->Checkpoint())
        {
            if (error)
                *error = "Checkpoint of " + db_->GetPath() + " failed.";
            return false;
        }
        return true;
    }

    void Session::Close()
    {
        if (in_transaction_)
        {
            in_transaction_ = false;
            db_->Rollback();
        }
    }

} // namespace sql
