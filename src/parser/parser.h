#pragma once

#include "lexer/lexer.h"
#include "parser/ast.h"
#include <memory>

namespace sql
{

    class Parser
    {
    public:
        explicit Parser(Lexer &lexer);

        std::unique_ptr<Statement> ParseStatement();

    private:
        void NextToken();
        Token Expect(TokenType type);
        bool Match(TokenType type);
        Token Peek() const { return current_token_; }

        std::unique_ptr<Statement> ParseCreate();
        std::unique_ptr<SelectStatement> ParseSelect();
        std::unique_ptr<CreateTableStatement> ParseCreateTable();
        std::unique_ptr<CreateIndexStatement> ParseCreateIndex();
        std::unique_ptr<DropTableStatement> ParseDropTable();
        std::unique_ptr<InsertStatement> ParseInsert();
        std::unique_ptr<DeleteStatement> ParseDelete();
        std::unique_ptr<UpdateStatement> ParseUpdate();

        std::unique_ptr<Expression> ParseExpression();
        std::unique_ptr<Expression> ParseTerm();
        std::unique_ptr<Expression> ParseComparison();
        std::unique_ptr<Expression> ParsePrimary();

        Lexer &lexer_;
        Token current_token_;
    };

} // namespace sql
