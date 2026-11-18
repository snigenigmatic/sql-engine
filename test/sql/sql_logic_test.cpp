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
// Each file runs in a fresh temporary database, statement by statement,
// through a Session (so BEGIN / COMMIT / ROLLBACK work). Every file runs
// in this process (Local), through a server over TCP (Remote) and on an
// in-process cluster of coordinators and storage nodes (Cluster), which
// must all give the same answers.

#include <gtest/gtest.h>
#include "cli/local_connection.h"
#include "net/remote_connection.h"
#include "server/server.h"
#include "session/session.h"
#ifdef SQLENGINE_DISTRIBUTED
#include "../cluster/test_cluster.h"
#endif
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
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
        protected:
            // Runs the file's checks over a connection to an empty database
            void RunScript(Connection &session);
        };

        void SqlLogicTest::RunScript(Connection &session)
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

        // A temporary database file (so transactions work), removed after
        struct TempDatabase
        {
            std::string path;
            std::unique_ptr<Database> db;

            TempDatabase()
                : path((std::filesystem::temp_directory_path() / ("sql_logic_" + std::to_string(getpid()) + ".db"))
                           .string())
            {
                Remove();
                std::string error;
                db = Database::Open(path, &error);
                EXPECT_NE(db, nullptr) << error;
            }
            ~TempDatabase()
            {
                db.reset();
                Remove();
            }
            void Remove()
            {
                std::filesystem::remove(path);
                std::filesystem::remove(path + "-wal");
            }
        };

        TEST_P(SqlLogicTest, Local)
        {
            TempDatabase file;
            ASSERT_NE(file.db, nullptr);
            Session session(file.db.get());
            LocalConnection connection(file.db.get(), &session);
            RunScript(connection);
        }

        TEST_P(SqlLogicTest, Remote)
        {
            TempDatabase file;
            ASSERT_NE(file.db, nullptr);
            Server server(file.db.get());
            server.Start("127.0.0.1", 0);
            net::RemoteConnection connection("127.0.0.1", server.Port());
            RunScript(connection);
        }

#ifdef SQLENGINE_DISTRIBUTED
        TEST_P(SqlLogicTest, Cluster)
        {
            cluster::testing::Cluster cluster;
            ASSERT_TRUE(cluster.WaitFormed());
            auto connection = cluster.Client(0);
            RunScript(*connection);
        }
#endif

        // A deployed cluster (deploy/): SQLENGINE_CLUSTER=host:port[,host:port...].
        // Every table is dropped before each file, so use a cluster of its own.
        TEST_P(SqlLogicTest, External)
        {
            const char *addresses = std::getenv("SQLENGINE_CLUSTER");
            if (!addresses || !*addresses)
                GTEST_SKIP() << "set SQLENGINE_CLUSTER to run against a deployed cluster";
            std::vector<std::pair<std::string, uint16_t>> servers;
            std::stringstream list(addresses);
            for (std::string one; std::getline(list, one, ',');)
            {
                const size_t colon = one.rfind(':');
                servers.emplace_back(one.substr(0, colon), static_cast<uint16_t>(std::stoi(one.substr(colon + 1))));
            }
            net::RemoteConnection connection(servers, true);
            auto drop_everything = [&]
            {
                std::stringstream tables(connection.Command("tables", ""));
                for (std::string line; std::getline(tables, line);)
                {
                    const size_t bracket = line.find(" [");
                    if (bracket != std::string::npos)
                    {
                        ASSERT_TRUE(connection.Execute("DROP TABLE " + line.substr(0, bracket) + ";").success) << line;
                    }
                }
            };
            drop_everything();
            RunScript(connection);
            drop_everything();
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
