#pragma once

#include <string>

namespace sql {

enum class TokenType {
    IDENTIFIER,
    KEYWORD,
    // ...
};

struct Token {
    TokenType type;
    std::string value;
};

} // namespace sql
