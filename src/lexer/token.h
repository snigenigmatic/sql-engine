#pragma once

#include <string>
#include <ostream>

namespace sql
{

    enum class TokenType
    {
        // Keywords
        SELECT,
        FROM,
        WHERE,
        INSERT,
        INTO,
        VALUES,
        DELETE,
        UPDATE,
        SET,
        CREATE,
        TABLE,
        INDEX,
        ON,
        INTEGER,
        VARCHAR,
        FLOAT,
        BOOLEAN,
        TRUE,
        FALSE,
        AND,
        OR,
        NOT,
        DROP,

        // Identifiers and Literals
        IDENTIFIER,
        STRING_LITERAL,
        INTEGER_LITERAL,
        FLOAT_LITERAL,

        // Operators
        PLUS,
        MINUS,
        STAR,
        SLASH,
        EQ,
        NEQ,
        LT,
        GT,
        LEQ,
        GEQ,

        // Delimiters
        LPAREN,
        RPAREN,
        COMMA,
        SEMICOLON,

        // System
        END_OF_FILE,
        ILLEGAL
    };

    struct Token
    {
        TokenType type;
        std::string value;
        size_t line = 0;
        size_t column = 0;
    };

    std::string TokenToString(TokenType type);
    std::ostream &operator<<(std::ostream &os, TokenType type);

} // namespace sql
