#include "parser/parser.h"
#include <stdexcept>
#include <vector>

namespace sql
{

    Parser::Parser(Lexer &lexer) : lexer_(lexer)
    {
        NextToken(); // Prime the pump
    }

    void Parser::NextToken()
    {
        current_token_ = lexer_.NextToken();
    }

    Token Parser::Expect(TokenType type)
    {
        if (current_token_.type == type)
        {
            Token token = current_token_;
            NextToken();
            return token;
        }
        throw std::runtime_error("Unexpected token: " + current_token_.value +
                                 " Expected type: " + TokenToString(type));
    }

    bool Parser::Match(TokenType type)
    {
        if (current_token_.type == type)
        {
            NextToken();
            return true;
        }
        return false;
    }

    std::unique_ptr<Statement> Parser::ParseStatement()
    {
        if (current_token_.type == TokenType::SELECT)
        {
            return ParseSelect();
        }
        throw std::runtime_error("Unexpected token at start of statement: " + current_token_.value);
    }

    std::unique_ptr<SelectStatement> Parser::ParseSelect()
    {
        auto stmt = std::make_unique<SelectStatement>();
        Expect(TokenType::SELECT);

        // Parse columns
        if (Match(TokenType::STAR))
        {
            stmt->select_star = true;
        }
        else
        {
            do
            {
                Token col = Expect(TokenType::IDENTIFIER);
                stmt->columns.push_back(col.value);
            } while (Match(TokenType::COMMA));
        }

        Expect(TokenType::FROM);
        Token table = Expect(TokenType::IDENTIFIER);
        stmt->table = table.value;

        if (Match(TokenType::WHERE))
        {
            stmt->where = ParseExpression();
        }

        Expect(TokenType::SEMICOLON);
        return stmt;
    }

    // Expression: OR
    std::unique_ptr<Expression> Parser::ParseExpression()
    {
        auto left = ParseTerm();

        while (current_token_.type == TokenType::OR)
        {
            TokenType op = current_token_.type;
            NextToken();
            auto right = ParseTerm();
            left = std::make_unique<BinaryExpression>(std::move(left), op, std::move(right));
        }
        return left;
    }

    // Term: AND
    std::unique_ptr<Expression> Parser::ParseTerm()
    {
        auto left = ParseComparison();

        while (current_token_.type == TokenType::AND)
        {
            TokenType op = current_token_.type;
            NextToken();
            auto right = ParseComparison();
            left = std::make_unique<BinaryExpression>(std::move(left), op, std::move(right));
        }
        return left;
    }

    // Comparison: =, <, >, etc.
    std::unique_ptr<Expression> Parser::ParseComparison()
    {
        auto left = ParsePrimary();

        // Check for comparison operators
        if (current_token_.type == TokenType::EQ ||
            current_token_.type == TokenType::NEQ ||
            current_token_.type == TokenType::LT ||
            current_token_.type == TokenType::GT ||
            current_token_.type == TokenType::LEQ ||
            current_token_.type == TokenType::GEQ)
        {

            TokenType op = current_token_.type;
            NextToken();
            auto right = ParsePrimary();
            left = std::make_unique<BinaryExpression>(std::move(left), op, std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expression> Parser::ParsePrimary()
    {
        Token t = current_token_;
        NextToken();

        switch (t.type)
        {
        case TokenType::IDENTIFIER:
            return std::make_unique<ColumnExpression>(t.value);
        case TokenType::INTEGER_LITERAL:
            return std::make_unique<LiteralExpression>(Value(std::stoi(t.value)));
        case TokenType::STRING_LITERAL:
            return std::make_unique<LiteralExpression>(Value(t.value));
        case TokenType::TRUE:
            return std::make_unique<LiteralExpression>(Value(true));
        case TokenType::FALSE:
            return std::make_unique<LiteralExpression>(Value(false));
        case TokenType::FLOAT_LITERAL:
            return std::make_unique<LiteralExpression>(Value(std::stod(t.value)));
        case TokenType::LPAREN:
        {
            // We already consumed LPAREN, so now parse expression inside
            auto expr = ParseExpression();
            Expect(TokenType::RPAREN);
            return expr;
        }
        default:
            throw std::runtime_error("Unexpected token in expression: " + t.value);
        }
    }

} // namespace sql
