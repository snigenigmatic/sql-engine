#pragma once

#include <string>
#include <vector>

namespace sql::cluster
{

    // The statements of a script, as text, each ending at its ';' (a ';'
    // inside a string literal does not end one). Text after the last ';'
    // that is not just whitespace is one more statement.
    std::vector<std::string> SplitStatements(const std::string &script);

} // namespace sql::cluster
