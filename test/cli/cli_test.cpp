#include <gtest/gtest.h>
#include "cli/shell.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace sql
{

    namespace
    {
        // A temporary database file, removed afterwards
        struct TempDatabase
        {
            std::string path;
            TempDatabase()
            {
                static int counter = 0;
                path = "/tmp/cli_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++) + ".db";
                std::remove(path.c_str());
                std::remove((path + "-wal").c_str());
            }
            ~TempDatabase()
            {
                std::remove(path.c_str());
                std::remove((path + "-wal").c_str());
            }
        };

        // A shell over a fresh database, writing to `out`
        struct TestShell
        {
            TempDatabase file;
            std::string error;
            std::unique_ptr<Database> db = Database::Open(file.path, &error);
            Session session{db.get()};
            std::ostringstream out;
            Shell shell{db.get(), &session, out};

            std::string Run(const std::vector<std::string> &lines)
            {
                out.str("");
                for (const auto &line : lines)
                    shell.HandleLine(line);
                return out.str();
            }
        };
    } // namespace

    TEST(CliTest, TablesAreSizedToTheirValues)
    {
        ExecutionResult result;
        result.success = true;
        result.column_names = {"id", "name", "score"};
        result.tuples.push_back(Tuple({Value(1), Value("ann"), Value(2.5)}));
        result.tuples.push_back(Tuple({Value(100), Value(DataType::VARCHAR), Value(0.1)}));
        EXPECT_EQ(FormatResult(result), " id | name | score\n"
                                        "----+------+------\n"
                                        "  1 | ann  |   2.5\n"
                                        "100 | NULL |   0.1\n"
                                        "(2 rows)\n");

        result.tuples.clear();
        EXPECT_EQ(FormatResult(result), "id | name | score\n---+------+------\n(0 rows)\n");

        result.success = false;
        result.message = "boom";
        EXPECT_EQ(FormatResult(result), "Error: boom\n");
        result.success = true;
        EXPECT_EQ(FormatResult(result), "boom\n"); // a statement's status message
    }

    TEST(CliTest, SchemaCanBeRunAgain)
    {
        TestShell a;
        a.Run({"CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(20) NOT NULL UNIQUE, note VARCHAR(5) "
               "DEFAULT 'it''s', x FLOAT DEFAULT 2.5, ok BOOLEAN DEFAULT FALSE, n INTEGER DEFAULT -3);",
               "CREATE INDEX t_x ON t (x);", "CREATE UNIQUE INDEX t_n ON t (n);"});
        const std::string schema = SchemaSQL(a.db->GetCatalog(), "t");
        EXPECT_EQ(schema, "CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(20) NOT NULL UNIQUE, note VARCHAR(5) "
                          "DEFAULT 'it''s', x FLOAT DEFAULT 2.5, ok BOOLEAN DEFAULT FALSE, n INTEGER DEFAULT -3);\n"
                          "CREATE UNIQUE INDEX t_n ON t (n);\n"
                          "CREATE INDEX t_x ON t (x);\n");

        // Running it recreates the same table
        TestShell b;
        std::istringstream lines(schema);
        std::string line;
        while (std::getline(lines, line))
            b.shell.HandleLine(line);
        EXPECT_EQ(SchemaSQL(b.db->GetCatalog(), "t"), schema);
        EXPECT_EQ(SchemaSQL(b.db->GetCatalog(), "nosuch"), "");
    }

    TEST(CliTest, StatementsSpanLinesAndCommandsStartWithADot)
    {
        TestShell t;
        EXPECT_EQ(t.shell.Prompt(), "sql> ");
        EXPECT_EQ(t.Run({"CREATE TABLE t (a INTEGER);", "INSERT INTO t", "VALUES (1),"}), "Table 't' created.\n");
        EXPECT_EQ(t.shell.Prompt(), "  -> ");
        EXPECT_EQ(t.Run({"(2);"}), "2 row(s) inserted.\n");
        EXPECT_EQ(t.Run({"SELECT a FROM t ORDER BY a DESC; SELECT COUNT(*) FROM t;"}),
                  "a\n-\n2\n1\n(2 rows)\nCOUNT(*)\n--------\n       2\n(1 row)\n");

        EXPECT_EQ(t.Run({".tables"}), "t (2 rows): a INTEGER\n");
        EXPECT_EQ(t.Run({"tables"}), "t (2 rows): a INTEGER\n"); // the old spelling
        EXPECT_EQ(t.Run({".schema t"}), "CREATE TABLE t (a INTEGER);\n");
        EXPECT_EQ(t.Run({".schema nosuch"}), "Error: no table named nosuch\n");
        EXPECT_NE(t.Run({".help"}).find(".read FILE"), std::string::npos);
        EXPECT_EQ(t.Run({".nope"}), "Unknown command: .nope (try .help)\n");

        t.Run({"BEGIN;"});
        EXPECT_EQ(t.shell.Prompt(), "sql*> ");
        EXPECT_NE(t.Run({".save"}).find("Error:"), std::string::npos); // not inside a transaction
        t.Run({"ROLLBACK;"});

        EXPECT_NE(t.Run({".timer on", "SELECT a FROM t WHERE a = 1;"}).find("Time: "), std::string::npos);
        EXPECT_EQ(t.Run({".timer off", "SELECT a FROM t WHERE a = 1;"}).find("Time: "), std::string::npos);
        EXPECT_EQ(t.Run({".timer maybe"}), "Usage: .timer on|off\n");

        EXPECT_TRUE(t.shell.HandleLine("SELECT 1 FROM t LIMIT 0;"));
        EXPECT_FALSE(t.shell.HandleLine(".quit"));
        EXPECT_FALSE(t.shell.HandleLine("exit"));
    }

    TEST(CliTest, ReadStopsAtTheFirstError)
    {
        char path[] = "/tmp/cli_test_XXXXXX.sql";
        const int fd = mkstemps(path, 4);
        ASSERT_GE(fd, 0);
        close(fd);
        {
            std::ofstream script(path);
            script << "CREATE TABLE t (a INTEGER PRIMARY KEY);\n"
                      "INSERT INTO t VALUES (1);\n"
                      "INSERT INTO t VALUES (1);\n" // duplicate key
                      "INSERT INTO t VALUES (2);\n";
        }
        TestShell t;
        EXPECT_FALSE(t.shell.ReadFile(path));
        const std::string output = t.out.str();
        EXPECT_NE(output.find("UNIQUE constraint failed"), std::string::npos) << output;
        EXPECT_NE(output.find("after statement 3"), std::string::npos) << output;
        EXPECT_EQ(t.Run({"SELECT COUNT(*) FROM t;"}), "COUNT(*)\n--------\n       1\n(1 row)\n");
        std::remove(path);

        EXPECT_FALSE(t.shell.ReadFile("/nonexistent/file.sql"));
        EXPECT_EQ(t.Run({".read /nonexistent/file.sql"}), "Error: cannot open /nonexistent/file.sql\n");
    }

} // namespace sql
