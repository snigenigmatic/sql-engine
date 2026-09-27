// Runs the golden SQL scripts in test/sql/*.test, a small subset of the
// sqllogictest format. Blocks are separated by blank lines; '#' starts a
// comment line.
//
//   statement ok              the SQL must succeed
//   statement error <text>    the SQL must fail with <text> in the message
//   query [nosort|rowsort]    the SQL's rows must equal the lines after
//   SELECT ...                "----", values joined by '|'; rowsort sorts
//   ----                      both sides first (for queries whose order is
//   1|ann                     not defined)
//
// Each file runs in a fresh in-memory database, statement by statement,
// through a Session (so BEGIN / COMMIT / ROLLBACK work).

#include <gtest/gtest.h>
#include "session/session.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace sql
{

    namespace
    {
        std::vector<std::string> TestFiles()
        {
            std::vector<std::string> files;
            for (const auto &entry : std::filesystem::directory_iterator(SQL_TEST_DIR))
            {
                if (entry.path().extension() == ".test")
                    files.push_back(entry.path().filename().string());
            }
            std::sort(files.begin(), files.end());
            return files;
        }

        std::string RowText(const Tuple &row)
        {
            std::string text;
            for (size_t i = 0; i < row.GetValueCount(); ++i)
                text += (i ? "|" : "") + row.GetValue(i).ToString();
            return text;
        }

        class SqlLogicTest : public ::testing::TestWithParam<std::string>
        {
        };

        TEST_P(SqlLogicTest, Script)
        {
            const std::string path = std::string(SQL_TEST_DIR) + "/" + GetParam();
            std::ifstream file(path);
            ASSERT_TRUE(file) << "cannot open " << path;
            std::vector<std::string> lines;
            for (std::string line; std::getline(file, line);)
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }

            // A real (temporary) database file, so transactions work
            const std::string db_path =
                (std::filesystem::temp_directory_path() / ("sql_logic_" + std::to_string(getpid()) + ".db")).string();
            std::filesystem::remove(db_path);
            std::filesystem::remove(db_path + "-wal");
            struct RemoveFiles // declared first, so it runs after the database closes
            {
                std::string path;
                ~RemoveFiles()
                {
                    std::filesystem::remove(path);
                    std::filesystem::remove(path + "-wal");
                }
            } cleanup{db_path};
            std::string error;
            auto db = Database::Open(db_path, &error);
            ASSERT_NE(db, nullptr) << error;
            Session session(db.get());
            size_t checks = 0;
            size_t i = 0;
            while (i < lines.size())
            {
                if (lines[i].empty() || lines[i][0] == '#')
                {
                    ++i;
                    continue;
                }
                const size_t header_line = i + 1; // 1-based, for messages
                const std::string header = lines[i++];
                const std::string where = GetParam() + ":" + std::to_string(header_line);

                // SQL lines, up to a blank line or "----"
                std::string sql;
                while (i < lines.size() && !lines[i].empty() && lines[i] != "----")
                    sql += (sql.empty() ? "" : "\n") + lines[i++];
                ASSERT_FALSE(sql.empty()) << where << ": no SQL after '" << header << "'";
                if (sql.back() != ';')
                    sql += ";";

                if (header.rfind("statement ok", 0) == 0)
                {
                    const ExecutionResult result = session.Execute(sql);
                    EXPECT_TRUE(result.success) << where << ": " << sql << "\n  failed: " << result.message;
                }
                else if (header.rfind("statement error", 0) == 0)
                {
                    const std::string expected = header.size() > 16 ? header.substr(16) : "";
                    const ExecutionResult result = session.Execute(sql);
                    EXPECT_FALSE(result.success) << where << ": " << sql << "\n  succeeded but should fail";
                    EXPECT_NE(result.message.find(expected), std::string::npos)
                        << where << ": " << sql << "\n  error was: " << result.message << "\n  expected: " << expected;
                }
                else if (header.rfind("query", 0) == 0)
                {
                    const bool rowsort = header.find("rowsort") != std::string::npos;
                    ASSERT_TRUE(i < lines.size() && lines[i] == "----") << where << ": query without ----";
                    ++i;
                    std::vector<std::string> expected;
                    while (i < lines.size() && !lines[i].empty())
                        expected.push_back(lines[i++]);

                    const ExecutionResult result = session.Execute(sql);
                    EXPECT_TRUE(result.success) << where << ": " << sql << "\n  failed: " << result.message;
                    std::vector<std::string> actual;
                    for (const auto &row : result.tuples)
                        actual.push_back(RowText(row));
                    if (rowsort)
                    {
                        std::sort(actual.begin(), actual.end());
                        std::sort(expected.begin(), expected.end());
                    }
                    EXPECT_EQ(actual, expected) << where << ": " << sql;
                }
                else
                {
                    FAIL() << where << ": unknown directive '" << header << "'";
                }
                ++checks;
            }
            EXPECT_GT(checks, 0u) << GetParam() << " has no checks";
        }

        INSTANTIATE_TEST_SUITE_P(Golden, SqlLogicTest, ::testing::ValuesIn(TestFiles()),
                                 [](const ::testing::TestParamInfo<std::string> &info)
                                 {
                                     std::string name = info.param.substr(0, info.param.find('.'));
                                     for (char &c : name)
                                     {
                                         if (!std::isalnum(static_cast<unsigned char>(c)))
                                             c = '_';
                                     }
                                     return name;
                                 });
    } // namespace

} // namespace sql
