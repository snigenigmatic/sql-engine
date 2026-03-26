#include "lexer/lexer.h"

#include "lexer/lexer.h"
#include <cctype>
#include <algorithm>
#include <map>

namespace sql
{

    Lexer::Lexer(const std::string &input) : input_(input)
    {
        ReadChar();
    }

    void Lexer::ReadChar()
    {
        if (read_position_ >= input_.length())
        {
            ch_ = 0;
        }
        else
        {
            ch_ = input_[read_position_];
        }
        position_ = read_position_;
        read_position_++;
        column_++;
    }

    char Lexer::PeekChar()
    {
        if (read_position_ >= input_.length())
        {
            return 0;
        }
        return input_[read_position_];
    }

    void Lexer::SkipWhitespace()
    {
        while (std::isspace(ch_))
        {
            if (ch_ == '\n')
            {
                line_++;
                column_ = 0;
            }
            ReadChar();
        }
    }

    Token Lexer::MakeToken(TokenType type, std::string literal)
    {
        Token token;
        token.type = type;
        token.value = literal;
        token.line = line_;
        token.column = column_ > literal.length() ? column_ - literal.length() : 0;
        return token;
    }

    static bool IsIdentifierChar(char c)
    {
        return std::isalnum(c) || c == '_';
    }

    Token Lexer::NextToken()
    {
        SkipWhitespace();

        Token token;
        std::string literal(1, ch_);

        switch (ch_)
        {
        case '=':
            token = MakeToken(TokenType::EQ, literal);
            break;
        case ';':
            token = MakeToken(TokenType::SEMICOLON, literal);
            break;
        case '(':
            token = MakeToken(TokenType::LPAREN, literal);
            break;
        case ')':
            token = MakeToken(TokenType::RPAREN, literal);
            break;
        case ',':
            token = MakeToken(TokenType::COMMA, literal);
            break;
        case '.':
            token = MakeToken(TokenType::DOT, literal);
            break;
        case '+':
            token = MakeToken(TokenType::PLUS, literal);
            break;
        case '-':
            token = MakeToken(TokenType::MINUS, literal);
            break;
        case '*':
            token = MakeToken(TokenType::STAR, literal);
            break;
        case '/':
            token = MakeToken(TokenType::SLASH, literal);
            break;
        case 0:
            token.type = TokenType::END_OF_FILE;
            token.value = "";
            return token;
        case '<':
            if (PeekChar() == '=')
            {
                char first = ch_;
                ReadChar();
                literal = std::string(1, first) + std::string(1, ch_);
                token = MakeToken(TokenType::LEQ, literal);
            }
            else if (PeekChar() == '>')
            {
                char first = ch_;
                ReadChar();
                literal = std::string(1, first) + std::string(1, ch_);
                token = MakeToken(TokenType::NEQ, literal);
            }
            else
            {
                token = MakeToken(TokenType::LT, literal);
            }
            break;
        case '>':
            if (PeekChar() == '=')
            {
                char first = ch_;
                ReadChar();
                literal = std::string(1, first) + std::string(1, ch_);
                token = MakeToken(TokenType::GEQ, literal);
            }
            else
            {
                token = MakeToken(TokenType::GT, literal);
            }
            break;
        case '!':
            if (PeekChar() == '=')
            {
                char first = ch_;
                ReadChar();
                literal = std::string(1, first) + std::string(1, ch_);
                token = MakeToken(TokenType::NEQ, literal);
            }
            else
            {
                token = MakeToken(TokenType::ILLEGAL, literal);
            }
            break;
        case '\'':
            token.type = TokenType::STRING_LITERAL;
            token.value = ReadString();
            token.line = line_;
            return token;
        default:
            if (std::isalpha(ch_) || ch_ == '_')
            {
                std::string ident = ReadIdentifier();
                token.type = LookupKeyword(ident);
                token.value = ident;
                token.line = line_;
                return token;
            }
            else if (std::isdigit(ch_))
            {
                std::string num = ReadNumber();
                if (num.find('.') != std::string::npos)
                {
                    token.type = TokenType::FLOAT_LITERAL;
                }
                else
                {
                    token.type = TokenType::INTEGER_LITERAL;
                }
                token.value = num;
                token.line = line_;
                return token;
            }
            else
            {
                token = MakeToken(TokenType::ILLEGAL, literal);
            }
            break;
        }

        ReadChar();
        return token;
    }

    std::string Lexer::ReadIdentifier()
    {
        size_t start = position_;
        while (IsIdentifierChar(ch_))
        {
            ReadChar();
        }
        return input_.substr(start, position_ - start);
    }

    std::string Lexer::ReadNumber()
    {
        size_t start = position_;
        while (std::isdigit(ch_))
        {
            ReadChar();
        }
        if (ch_ == '.')
        {
            ReadChar();
            while (std::isdigit(ch_))
            {
                ReadChar();
            }
        }
        return input_.substr(start, position_ - start);
    }

    std::string Lexer::ReadString()
    {
        size_t start = position_ + 1;
        ReadChar();
        while (ch_ != '\'' && ch_ != 0)
        {
            ReadChar();
        }

        std::string str = input_.substr(start, position_ - start);
        if (ch_ == '\'')
        {
            ReadChar();
        }
        return str;
    }

    TokenType Lexer::LookupKeyword(const std::string &ident)
    {
        std::string upper_ident = ident;
        std::transform(upper_ident.begin(), upper_ident.end(), upper_ident.begin(), ::toupper);

        static const std::map<std::string, TokenType> keywords = {
            {"SELECT", TokenType::SELECT},
            {"FROM", TokenType::FROM},
            {"WHERE", TokenType::WHERE},
            {"INSERT", TokenType::INSERT},
            {"INTO", TokenType::INTO},
            {"VALUES", TokenType::VALUES},
            {"DELETE", TokenType::DELETE},
            {"UPDATE", TokenType::UPDATE},
            {"SET", TokenType::SET},
            {"CREATE", TokenType::CREATE},
            {"TABLE", TokenType::TABLE},
            {"INDEX", TokenType::INDEX},
            {"ON", TokenType::ON},
            {"INT", TokenType::INTEGER},
            {"INTEGER", TokenType::INTEGER},
            {"VARCHAR", TokenType::VARCHAR},
            {"FLOAT", TokenType::FLOAT},
            {"BOOLEAN", TokenType::BOOLEAN},
            {"AND", TokenType::AND},
            {"OR", TokenType::OR},
             {"NOT", TokenType::NOT},
             {"DROP", TokenType::DROP},
             {"JOIN", TokenType::JOIN},
             {"INNER", TokenType::INNER},
             {"EXPLAIN", TokenType::EXPLAIN},
             {"TRUE", TokenType::TRUE},
             {"FALSE", TokenType::FALSE}};


        auto it = keywords.find(upper_ident);
        if (it != keywords.end())
        {
            return it->second;
        }
        return TokenType::IDENTIFIER;
    }

} // namespace sql
