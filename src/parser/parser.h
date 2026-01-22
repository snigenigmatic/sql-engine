#pragma once

#include "lexer/lexer.h"
#include "parser/ast.h"
#include <memory>

namespace sql {

class Parser {
public:
    explicit Parser(Lexer& lexer);
    
    // Parser methods
    
private:
    Lexer& lexer_;
};

} // namespace sql
