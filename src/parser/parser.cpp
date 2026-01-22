#include "parser/parser.h"

namespace sql {

Parser::Parser(Lexer& lexer) : lexer_(lexer) {}

} // namespace sql
