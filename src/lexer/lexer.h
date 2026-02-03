#pragma once

#include <string>
#include "lexer/token.h"

namespace sql
{

    class Lexer
    {
    public:
        explicit Lexer(const std::string &input);

        Token NextToken();

    private:
        void ReadChar();
        char PeekChar();
        void SkipWhitespace();

        Token MakeToken(TokenType type, std::string literal);

        std::string ReadIdentifier();
        std::string ReadNumber();
        std::string ReadString();

        TokenType LookupKeyword(const std::string &ident);

        std::string input_;
        size_t position_ = 0;      // current position in input (identifies current char)
        size_t read_position_ = 0; // current reading position in input (after current char)
        char ch_ = 0;              // current char under examination

        // Line/Col tracking
        size_t line_ = 1;
        size_t column_ = 0;
    };

} // namespace sql
