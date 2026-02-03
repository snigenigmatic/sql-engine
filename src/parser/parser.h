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
        // Helper methods
        void NextToken();
        Token Expect(TokenType type);
        bool Match(TokenType type);
        Token Peek() const { return current_token_; }

        // Parsing methods
        std::unique_ptr<SelectStatement> ParseSelect();

        // Expression parsing (Precedence climbing)
        std::unique_ptr<Expression> ParseExpression(); // Handles OR
        std::unique_ptr<Expression> ParseTerm();       // Handles AND
        std::unique_ptr<Expression> ParseComparison(); // Handles <, >, =
        std::unique_ptr<Expression> ParsePrimary();    // Literals, Columns, ()

        Lexer &lexer_;
        Token current_token_;
    };

} // namespace sql
