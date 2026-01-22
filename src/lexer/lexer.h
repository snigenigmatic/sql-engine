#pragma once

#include <string>

namespace sql {

class Lexer {
public:
    explicit Lexer(const std::string& input);
    
    // Lexer methods
    
private:
    std::string input_;
};

} // namespace sql
