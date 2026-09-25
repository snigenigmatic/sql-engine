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
            if (!parser.AtEnd())
                return Error("Expected a single statement; use ExecuteScript for several.");
        }
        catch (const std::exception &e)
        {
            return Error(std::string("Parse error: ") + e.what());
        }
        if (!stmt)
            return Error("Failed to parse SQL statement.");
        return Execute(stmt.get());
    }

    std::vector<ExecutionResult> Session::ExecuteScript(const std::string &sql)
    {
        std::vector<ExecutionResult> results;
        Lexer lexer(sql);
        std::unique_ptr<Parser> parser;
        try
        {
            parser = std::make_unique<Parser>(lexer);
        }
        catch (const std::exception &e)
        {
            results.push_back(Error(std::string("Parse error: ") + e.what()));
            return results;
        }
        while (!parser->AtEnd())
        {
            std::unique_ptr<Statement> stmt;
            try
            {
                stmt = parser->ParseStatement();
            }
            catch (const std::exception &e)
            {
                results.push_back(Error(std::string("Parse error: ") + e.what()));
                break;
            }
            results.push_back(Execute(stmt.get()));
        }
        return results;
    }

    ExecutionResult Session::Execute(Statement *stmt)
    {
        if (db_->IsBroken())
            return Error("The database hit an error it could not roll back; reopen it to recover the "
                         "last committed state.");

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
                {
                    result.message += " (statement rolled back; transaction still open)";
                }
                else
                {
                    // The failed statement's changes could still be committed
                    db_->MarkBroken();
                    result.message += " (" + error + ")";
                }
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
            {
                result.message += " (statement rolled back)";
            }
            else
            {
                // The next commit would include the failed statement
                db_->MarkBroken();
                result.message += " (" + error + ")";
            }
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
        {
            if (!in_transaction_)
                return Error("No transaction is in progress.");
            if (db_->Commit())
            {
                in_transaction_ = false;
                return Ok("Transaction committed.");
            }
            std::string error;
            if (db_->Rollback(&error))
            {
                in_transaction_ = false;
                return Error("Commit failed; the transaction was rolled back.");
            }
            // Nothing may commit this transaction's partial work, not even
            // closing the database
            db_->MarkBroken();
            return Error("Commit failed and the transaction could not be rolled back (" + error +
                         "); reopen the database to recover the last committed state.");
        }

        case TransactionStatement::Kind::ROLLBACK:
        {
            if (!in_transaction_)
                return Error("No transaction is in progress.");
            std::string error;
            if (!db_->Rollback(&error))
                return Error(error); // still open: ROLLBACK can be retried
            in_transaction_ = false;
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
            // If the rollback fails, closing the database must not commit
            // the transaction instead
            if (!db_->IsBroken() && !db_->Rollback())
                db_->MarkBroken();
        }
    }

} // namespace sql
