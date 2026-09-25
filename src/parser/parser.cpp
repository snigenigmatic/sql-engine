#include "parser/parser.h"
#include <cctype>
#include <climits>
#include <stdexcept>
#include <vector>

namespace sql
{

    Parser::Parser(Lexer &lexer) : lexer_(lexer)
    {
        NextToken();
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
        switch (current_token_.type)
        {
        case TokenType::SELECT:
            return ParseSelect();
        case TokenType::CREATE:
            return ParseCreate();
        case TokenType::INSERT:
            return ParseInsert();
        case TokenType::DELETE:
            return ParseDelete();
        case TokenType::UPDATE:
            return ParseUpdate();
        case TokenType::DROP:
            return ParseDropTable();
        case TokenType::EXPLAIN:
            return ParseExplain();
        case TokenType::BEGIN:
        case TokenType::COMMIT:
        case TokenType::ROLLBACK:
            return ParseTransaction();
        default:
            throw std::runtime_error("Unexpected token at start of statement: " + current_token_.value);
        }
    }

    std::unique_ptr<SelectStatement> Parser::ParseSelect()
    {
        auto stmt = std::make_unique<SelectStatement>();
        Expect(TokenType::SELECT);

        if (Match(TokenType::STAR))
        {
            stmt->select_star = true;
        }
        else
        {
            do
            {
                SelectItem item;
                item.expr = ParseExpression();
                // expr AS alias, or expr alias
                if (Match(TokenType::AS))
                    item.alias = Expect(TokenType::IDENTIFIER).value;
                else if (current_token_.type == TokenType::IDENTIFIER)
                    item.alias = Expect(TokenType::IDENTIFIER).value;
                stmt->items.push_back(std::move(item));
            } while (Match(TokenType::COMMA));

            if (!stmt->HasComputedItems())
            {
                for (const auto &item : stmt->items)
                    stmt->columns.push_back(static_cast<const ColumnExpression *>(item.expr.get())->name);
            }
        }

        Expect(TokenType::FROM);
        Token table = Expect(TokenType::IDENTIFIER);
        stmt->table = table.value;

        bool has_join = false;
        if (Match(TokenType::INNER))
        {
            Expect(TokenType::JOIN);
            has_join = true;
        }
        else if (Match(TokenType::JOIN))
        {
            has_join = true;
        }

        if (has_join)
        {
            Token join_table = Expect(TokenType::IDENTIFIER);
            stmt->join_table = join_table.value;
            Expect(TokenType::ON);
            std::string left = ParseQualifiedColumnName();
            Expect(TokenType::EQ);
            std::string right = ParseQualifiedColumnName();
            stmt->join_left_column = left;
            stmt->join_right_column = right;
        }

        if (Match(TokenType::WHERE))
        {
            stmt->where = ParseExpression();
        }

        Expect(TokenType::SEMICOLON);
        return stmt;
    }

    std::unique_ptr<Statement> Parser::ParseCreate()
    {
        Expect(TokenType::CREATE);
        if (current_token_.type == TokenType::TABLE)
            return ParseCreateTable();
        if (current_token_.type == TokenType::INDEX)
            return ParseCreateIndex();
        if (Match(TokenType::UNIQUE))
        {
            auto index = ParseCreateIndex();
            index->unique = true;
            return index;
        }
        throw std::runtime_error("Expected TABLE, INDEX or UNIQUE INDEX after CREATE");
    }

    // NOT NULL | NULL | PRIMARY KEY | UNIQUE | DEFAULT literal, in any order
    void Parser::ParseColumnConstraints(ColumnDef *col)
    {
        while (true)
        {
            if (Match(TokenType::NOT))
            {
                Expect(TokenType::NULL_KW);
                col->not_null = true;
            }
            else if (Match(TokenType::NULL_KW))
            {
                // explicitly nullable: the default
            }
            else if (Match(TokenType::PRIMARY))
            {
                // KEY is not reserved (columns may be called "key")
                Token key = Expect(TokenType::IDENTIFIER);
                std::string upper = key.value;
                for (char &c : upper)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (upper != "KEY")
                    throw std::runtime_error("Expected KEY after PRIMARY, got: " + key.value);
                col->primary_key = true;
            }
            else if (Match(TokenType::UNIQUE))
            {
                col->unique = true;
            }
            else if (Match(TokenType::DEFAULT))
            {
                auto expr = ParsePrimary();
                if (expr->GetType() != ExpressionType::LITERAL)
                    throw std::runtime_error("DEFAULT must be a literal value");
                col->default_value = static_cast<LiteralExpression *>(expr.get())->value;
            }
            else
            {
                return;
            }
        }
    }

    std::unique_ptr<CreateTableStatement> Parser::ParseCreateTable()
    {
        auto stmt = std::make_unique<CreateTableStatement>();
        Expect(TokenType::TABLE);

        Token name = Expect(TokenType::IDENTIFIER);
        stmt->table = name.value;

        Expect(TokenType::LPAREN);

        do
        {
            ColumnDef col;
            Token col_name = Expect(TokenType::IDENTIFIER);
            col.name = col_name.value;

            // Type
            Token type_tok = current_token_;
            if (type_tok.type == TokenType::INTEGER || type_tok.type == TokenType::FLOAT ||
                type_tok.type == TokenType::BOOLEAN || type_tok.type == TokenType::VARCHAR)
            {
                col.type_token = type_tok.type;
                NextToken();
            }
            else
            {
                throw std::runtime_error("Expected data type, got: " + type_tok.value);
            }

            // VARCHAR(n)
            if (col.type_token == TokenType::VARCHAR && current_token_.type == TokenType::LPAREN)
            {
                NextToken(); // consume (
                Token len = Expect(TokenType::INTEGER_LITERAL);
                col.length = std::stoi(len.value);
                Expect(TokenType::RPAREN);
            }

            ParseColumnConstraints(&col);
            stmt->columns.push_back(std::move(col));
        } while (Match(TokenType::COMMA));

        Expect(TokenType::RPAREN);
        Expect(TokenType::SEMICOLON);
        return stmt;
    }

    std::unique_ptr<CreateIndexStatement> Parser::ParseCreateIndex()
    {
        auto stmt = std::make_unique<CreateIndexStatement>();
        Expect(TokenType::INDEX);
        Token name = Expect(TokenType::IDENTIFIER);
        stmt->index_name = name.value;
        Expect(TokenType::ON);
        Token table = Expect(TokenType::IDENTIFIER);
        stmt->table = table.value;
        Expect(TokenType::LPAREN);
        Token col = Expect(TokenType::IDENTIFIER);
        stmt->column = col.value;
        Expect(TokenType::RPAREN);
        Expect(TokenType::SEMICOLON);
        return stmt;
    }

    std::unique_ptr<InsertStatement> Parser::ParseInsert()
    {
        auto stmt = std::make_unique<InsertStatement>();
        Expect(TokenType::INSERT);
        Expect(TokenType::INTO);

        Token table = Expect(TokenType::IDENTIFIER);
        stmt->table = table.value;

        // Optional column list: INSERT INTO t (a, b) VALUES ...
        if (Match(TokenType::LPAREN))
        {
            do
            {
                stmt->columns.push_back(Expect(TokenType::IDENTIFIER).value);
            } while (Match(TokenType::COMMA));
            Expect(TokenType::RPAREN);
        }

        Expect(TokenType::VALUES);

        // Parse one or more value lists: (v1, v2), (v3, v4)
        do
        {
            Expect(TokenType::LPAREN);
            std::vector<std::unique_ptr<Expression>> row;
            do
            {
                row.push_back(ParseExpression());
            } while (Match(TokenType::COMMA));
            Expect(TokenType::RPAREN);
            stmt->rows.push_back(std::move(row));
        } while (Match(TokenType::COMMA));

        Expect(TokenType::SEMICOLON);
        return stmt;
    }

    std::unique_ptr<DeleteStatement> Parser::ParseDelete()
    {
        auto stmt = std::make_unique<DeleteStatement>();
        Expect(TokenType::DELETE);
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

    std::unique_ptr<UpdateStatement> Parser::ParseUpdate()
    {
        auto stmt = std::make_unique<UpdateStatement>();
        Expect(TokenType::UPDATE);

        Token table = Expect(TokenType::IDENTIFIER);
        stmt->table = table.value;

        Expect(TokenType::SET);

        do
        {
            Token col = Expect(TokenType::IDENTIFIER);
            Expect(TokenType::EQ);
            auto expr = ParseExpression();
            stmt->assignments.emplace_back(col.value, std::move(expr));
        } while (Match(TokenType::COMMA));

        if (Match(TokenType::WHERE))
        {
            stmt->where = ParseExpression();
        }

        Expect(TokenType::SEMICOLON);
        return stmt;
    }

    std::unique_ptr<TransactionStatement> Parser::ParseTransaction()
    {
        TransactionStatement::Kind kind;
        if (Match(TokenType::BEGIN))
            kind = TransactionStatement::Kind::BEGIN;
        else if (Match(TokenType::COMMIT))
            kind = TransactionStatement::Kind::COMMIT;
        else
        {
            Expect(TokenType::ROLLBACK);
            kind = TransactionStatement::Kind::ROLLBACK;
        }
        Match(TokenType::TRANSACTION); // optional
        Expect(TokenType::SEMICOLON);
        return std::make_unique<TransactionStatement>(kind);
    }

    std::unique_ptr<DropTableStatement> Parser::ParseDropTable()
    {
        auto stmt = std::make_unique<DropTableStatement>();
        Expect(TokenType::DROP);
        Expect(TokenType::TABLE);
        Token name = Expect(TokenType::IDENTIFIER);
        stmt->table = name.value;
        Expect(TokenType::SEMICOLON);
        return stmt;
    }

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

    std::unique_ptr<Expression> Parser::ParseTerm()
    {
        auto left = ParseNot();
        while (current_token_.type == TokenType::AND)
        {
            TokenType op = current_token_.type;
            NextToken();
            auto right = ParseNot();
            left = std::make_unique<BinaryExpression>(std::move(left), op, std::move(right));
        }
        return left;
    }

    // NOT binds tighter than AND / OR and looser than comparisons
    std::unique_ptr<Expression> Parser::ParseNot()
    {
        if (Match(TokenType::NOT))
            return std::make_unique<UnaryExpression>(TokenType::NOT, ParseNot());
        return ParseComparison();
    }

    std::unique_ptr<Expression> Parser::ParseComparison()
    {
        auto left = ParseAdditive();
        if (Match(TokenType::IS))
        {
            const bool negated = Match(TokenType::NOT);
            Expect(TokenType::NULL_KW);
            return std::make_unique<IsNullExpression>(std::move(left), negated);
        }

        // [NOT] LIKE / IN / BETWEEN
        const bool negated = Match(TokenType::NOT);
        if (Match(TokenType::LIKE))
            return std::make_unique<LikeExpression>(std::move(left), ParseAdditive(), negated);
        if (Match(TokenType::IN))
        {
            Expect(TokenType::LPAREN);
            std::vector<std::unique_ptr<Expression>> list;
            do
            {
                list.push_back(ParseExpression());
            } while (Match(TokenType::COMMA));
            Expect(TokenType::RPAREN);
            return std::make_unique<InListExpression>(std::move(left), std::move(list), negated);
        }
        if (Match(TokenType::BETWEEN))
        {
            // The AND here separates the bounds; it is not a logical AND
            auto low = ParseAdditive();
            Expect(TokenType::AND);
            auto high = ParseAdditive();
            return std::make_unique<BetweenExpression>(std::move(left), std::move(low), std::move(high), negated);
        }
        if (negated)
            throw std::runtime_error("Expected LIKE, IN or BETWEEN after NOT, got: " + current_token_.value);

        if (current_token_.type == TokenType::EQ ||
            current_token_.type == TokenType::NEQ ||
            current_token_.type == TokenType::LT ||
            current_token_.type == TokenType::GT ||
            current_token_.type == TokenType::LEQ ||
            current_token_.type == TokenType::GEQ)
        {
            TokenType op = current_token_.type;
            NextToken();
            auto right = ParseAdditive();
            left = std::make_unique<BinaryExpression>(std::move(left), op, std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expression> Parser::ParseAdditive()
    {
        auto left = ParseMultiplicative();
        while (current_token_.type == TokenType::PLUS || current_token_.type == TokenType::MINUS)
        {
            TokenType op = current_token_.type;
            NextToken();
            left = std::make_unique<BinaryExpression>(std::move(left), op, ParseMultiplicative());
        }
        return left;
    }

    std::unique_ptr<Expression> Parser::ParseMultiplicative()
    {
        auto left = ParseUnary();
        while (current_token_.type == TokenType::STAR || current_token_.type == TokenType::SLASH)
        {
            TokenType op = current_token_.type;
            NextToken();
            left = std::make_unique<BinaryExpression>(std::move(left), op, ParseUnary());
        }
        return left;
    }

    std::unique_ptr<Expression> Parser::ParseUnary()
    {
        if (Match(TokenType::MINUS))
        {
            auto operand = ParseUnary();
            // Fold "-5" into a literal so it stays usable as an index key
            if (operand->GetType() == ExpressionType::LITERAL)
            {
                const Value &v = static_cast<LiteralExpression *>(operand.get())->value;
                if (!v.IsNull() && v.GetType() == DataType::INTEGER && v.GetAsInt() != INT32_MIN)
                    return std::make_unique<LiteralExpression>(Value(-v.GetAsInt()));
                if (!v.IsNull() && v.GetType() == DataType::FLOAT)
                    return std::make_unique<LiteralExpression>(Value(-v.GetAsFloat()));
            }
            return std::make_unique<UnaryExpression>(TokenType::MINUS, std::move(operand));
        }
        return ParsePrimary();
    }

    std::unique_ptr<Expression> Parser::ParsePrimary()
    {
        Token t = current_token_;
        NextToken();

        switch (t.type)
        {
        case TokenType::IDENTIFIER:
        {
            std::string name = t.value;
            if (current_token_.type == TokenType::DOT)
            {
                NextToken(); // consume dot
                Token rhs = Expect(TokenType::IDENTIFIER);
                name += "." + rhs.value;
            }
            return std::make_unique<ColumnExpression>(name);
        }
        case TokenType::INTEGER_LITERAL:
            return std::make_unique<LiteralExpression>(Value(std::stoi(t.value)));
        case TokenType::STRING_LITERAL:
            return std::make_unique<LiteralExpression>(Value(t.value));
        case TokenType::TRUE:
            return std::make_unique<LiteralExpression>(Value(true));
        case TokenType::FALSE:
            return std::make_unique<LiteralExpression>(Value(false));
        case TokenType::NULL_KW:
            return std::make_unique<LiteralExpression>(Value()); // untyped NULL
        case TokenType::FLOAT_LITERAL:
            return std::make_unique<LiteralExpression>(Value(std::stod(t.value)));
        case TokenType::LPAREN:
        {
            auto expr = ParseExpression();
            Expect(TokenType::RPAREN);
            return expr;
        }
        // Allow negative number literals
        case TokenType::MINUS:
        {
            Token num = current_token_;
            NextToken();
            if (num.type == TokenType::INTEGER_LITERAL)
                return std::make_unique<LiteralExpression>(Value(-std::stoi(num.value)));
            if (num.type == TokenType::FLOAT_LITERAL)
                return std::make_unique<LiteralExpression>(Value(-std::stod(num.value)));
            throw std::runtime_error("Expected number after '-'");
        }
        default:
            throw std::runtime_error("Unexpected token in expression: " + t.value);
        }
    }

    std::string Parser::ParseQualifiedColumnName()
    {
        Token first = Expect(TokenType::IDENTIFIER);
        std::string name = first.value;
        if (Match(TokenType::DOT))
        {
            Token second = Expect(TokenType::IDENTIFIER);
            name += "." + second.value;
        }
        return name;
    }

    std::unique_ptr<ExplainStatement> Parser::ParseExplain()
    {
        Expect(TokenType::EXPLAIN);
        auto stmt = std::make_unique<ExplainStatement>();
        stmt->select = ParseSelect();
        return stmt;
    }

} // namespace sql
