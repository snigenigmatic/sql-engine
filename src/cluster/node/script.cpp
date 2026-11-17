#include "cluster/node/script.h"
#include "lexer/lexer.h"

namespace sql::cluster
{

    namespace
    {
        std::string Trim(const std::string &text)
        {
            const size_t start = text.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                return "";
            return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
        }
    } // namespace

    std::vector<std::string> SplitStatements(const std::string &script)
    {
        // Offset of each line's first character (the lexer counts lines from 1)
        std::vector<size_t> line_starts = {0};
        for (size_t i = 0; i < script.size(); ++i)
            if (script[i] == '\n')
                line_starts.push_back(i + 1);

        std::vector<std::string> statements;
        size_t start = 0;
        Lexer lexer(script);
        for (Token token = lexer.NextToken(); token.type != TokenType::END_OF_FILE; token = lexer.NextToken())
        {
            if (token.type != TokenType::SEMICOLON || token.line == 0 || token.line > line_starts.size())
                continue;
            const size_t end = line_starts[token.line - 1] + token.column + 1;
            const std::string statement = Trim(script.substr(start, end - start));
            if (!statement.empty() && statement != ";")
                statements.push_back(statement);
            start = end;
        }
        const std::string rest = Trim(start < script.size() ? script.substr(start) : "");
        if (!rest.empty())
            statements.push_back(rest);
        return statements;
    }

} // namespace sql::cluster
