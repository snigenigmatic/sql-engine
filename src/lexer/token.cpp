#include "lexer/token.h"

namespace sql
{

    std::string TokenToString(TokenType type)
    {
        switch (type)
        {
        case TokenType::SELECT: return "SELECT";
        case TokenType::FROM: return "FROM";
        case TokenType::WHERE: return "WHERE";
        case TokenType::INSERT: return "INSERT";
        case TokenType::INTO: return "INTO";
        case TokenType::VALUES: return "VALUES";
        case TokenType::DELETE: return "DELETE";
        case TokenType::UPDATE: return "UPDATE";
        case TokenType::SET: return "SET";
        case TokenType::CREATE: return "CREATE";
        case TokenType::TABLE: return "TABLE";
        case TokenType::INDEX: return "INDEX";
        case TokenType::ON: return "ON";
        case TokenType::INTEGER: return "INTEGER";
        case TokenType::VARCHAR: return "VARCHAR";
        case TokenType::FLOAT: return "FLOAT";
        case TokenType::BOOLEAN: return "BOOLEAN";
        case TokenType::TRUE: return "TRUE";
        case TokenType::FALSE: return "FALSE";
        case TokenType::AND: return "AND";
        case TokenType::OR: return "OR";
        case TokenType::NOT: return "NOT";
        case TokenType::DROP: return "DROP";
        case TokenType::JOIN: return "JOIN";
        case TokenType::EXPLAIN: return "EXPLAIN";
        case TokenType::INNER: return "INNER";
        case TokenType::BEGIN: return "BEGIN";
        case TokenType::COMMIT: return "COMMIT";
        case TokenType::ROLLBACK: return "ROLLBACK";
        case TokenType::TRANSACTION: return "TRANSACTION";
        case TokenType::NULL_KW: return "NULL";
        case TokenType::IS: return "IS";
        case TokenType::PRIMARY: return "PRIMARY";
        case TokenType::UNIQUE: return "UNIQUE";
        case TokenType::DEFAULT: return "DEFAULT";
        case TokenType::AS: return "AS";
        case TokenType::LIKE: return "LIKE";
        case TokenType::IN: return "IN";
        case TokenType::BETWEEN: return "BETWEEN";
        case TokenType::ORDER: return "ORDER";
        case TokenType::BY: return "BY";
        case TokenType::ASC: return "ASC";
        case TokenType::DESC: return "DESC";
        case TokenType::LIMIT: return "LIMIT";
        case TokenType::OFFSET: return "OFFSET";
        case TokenType::DISTINCT: return "DISTINCT";
        case TokenType::GROUP: return "GROUP";
        case TokenType::HAVING: return "HAVING";
        case TokenType::IDENTIFIER: return "IDENTIFIER";
        case TokenType::STRING_LITERAL: return "STRING_LITERAL";
        case TokenType::INTEGER_LITERAL: return "INTEGER_LITERAL";
        case TokenType::FLOAT_LITERAL: return "FLOAT_LITERAL";
        case TokenType::PLUS: return "PLUS";
        case TokenType::MINUS: return "MINUS";
        case TokenType::STAR: return "STAR";
        case TokenType::SLASH: return "SLASH";
        case TokenType::EQ: return "EQ";
        case TokenType::NEQ: return "NEQ";
        case TokenType::LT: return "LT";
        case TokenType::GT: return "GT";
        case TokenType::LEQ: return "LEQ";
        case TokenType::GEQ: return "GEQ";
        case TokenType::LPAREN: return "LPAREN";
        case TokenType::RPAREN: return "RPAREN";
        case TokenType::COMMA: return "COMMA";
        case TokenType::DOT: return "DOT";
        case TokenType::SEMICOLON: return "SEMICOLON";
        case TokenType::END_OF_FILE: return "EOF";
        case TokenType::ILLEGAL: return "ILLEGAL";
        default: return "UNKNOWN";
        }
    }

    std::ostream &operator<<(std::ostream &os, TokenType type)
    {
        return os << TokenToString(type);
    }

} // namespace sql
