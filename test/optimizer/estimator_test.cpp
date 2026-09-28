#include <gtest/gtest.h>
#include "catalog/catalog.h"
#include "execution/executor.h"
#include "lexer/lexer.h"
#include "optimizer/estimator.h"
#include "parser/parser.h"

namespace sql
{

    namespace
    {
        std::unique_ptr<Statement> Parse(const std::string &sql)
        {
            Lexer lexer(sql);
            Parser parser(lexer);
            return parser.ParseStatement();
        }

        void Exec(Catalog &catalog, const std::string &sql)
        {
            auto stmt = Parse(sql);
            Executor executor(&catalog);
            auto result = executor.Execute(stmt.get());
            ASSERT_TRUE(result.success) << sql << ": " << result.message;
        }

        // t: 100 rows; id 1..100 (unique), g in 0..3, v NULL in 10 rows
        class EstimatorTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                Exec(catalog_, "CREATE TABLE t (id INTEGER, g INTEGER, v FLOAT, s VARCHAR(5));");
                std::string insert = "INSERT INTO t VALUES ";
                for (int i = 1; i <= 100; ++i)
                    insert += (i > 1 ? ", (" : "(") + std::to_string(i) + ", " + std::to_string(i % 4) + ", " +
                              (i <= 10 ? std::string("NULL") : std::to_string(i) + ".0") + ", 'x')";
                Exec(catalog_, insert + ";");
                Exec(catalog_, "CREATE TABLE u (id INTEGER, t_id INTEGER);");
                Exec(catalog_, "INSERT INTO u VALUES (1, 1), (2, 1), (3, 2);");
            }

            // Selectivity of the WHERE of "SELECT * FROM t, u WHERE <condition>"
            double Selectivity(const std::string &condition)
            {
                auto stmt = Parse("SELECT * FROM t, u WHERE " + condition + ";");
                const auto *select = static_cast<const SelectStatement *>(stmt.get());
                const Scope scope = Scope::ForSelect(*select, &catalog_);
                return Estimator(scope, &catalog_).Selectivity(select->where.get());
            }

            Catalog catalog_;
        };
    } // namespace

    TEST_F(EstimatorTest, DefaultsWithoutStatistics)
    {
        EXPECT_DOUBLE_EQ(Selectivity("t.g = 1"), Estimator::DEFAULT_EQUALITY);
        EXPECT_DOUBLE_EQ(Selectivity("t.id > 5"), Estimator::DEFAULT_RANGE);
        EXPECT_DOUBLE_EQ(Selectivity("5 < t.id"), Estimator::DEFAULT_RANGE);
        EXPECT_DOUBLE_EQ(Selectivity("t.v IS NULL"), Estimator::DEFAULT_IS_NULL);
        EXPECT_DOUBLE_EQ(Selectivity("t.s LIKE 'x%'"), Estimator::DEFAULT_LIKE);
        EXPECT_DOUBLE_EQ(Selectivity("t.g = 1 AND t.id > 5"), Estimator::DEFAULT_EQUALITY * Estimator::DEFAULT_RANGE);
        EXPECT_NEAR(Selectivity("t.g = 1 OR t.g = 2"), 0.1 + 0.1 - 0.01, 1e-12);
        EXPECT_DOUBLE_EQ(Selectivity("NOT (t.g = 1)"), 0.9);
        EXPECT_DOUBLE_EQ(Selectivity("t.g = NULL"), 0.0); // never true
        // A unique index says a key matches at most one row
        EXPECT_DOUBLE_EQ(Selectivity("t.id = 7"), Estimator::DEFAULT_EQUALITY);
        Exec(catalog_, "CREATE UNIQUE INDEX t_id ON t (id);");
        EXPECT_DOUBLE_EQ(Selectivity("t.id = 7"), 0.01);
        EXPECT_DOUBLE_EQ(Selectivity("t.g = 1"), Estimator::DEFAULT_EQUALITY);
        // Both defaults below the threshold: indexes are used as before ANALYZE
        EXPECT_LE(Estimator::DEFAULT_EQUALITY, Estimator::INDEX_THRESHOLD);
        EXPECT_LE(Estimator::DEFAULT_RANGE, Estimator::INDEX_THRESHOLD);
    }

    TEST_F(EstimatorTest, UsesStatistics)
    {
        ASSERT_TRUE(catalog_.Analyze("t"));
        EXPECT_DOUBLE_EQ(Selectivity("t.id = 7"), 0.01);        // 100 distinct
        EXPECT_DOUBLE_EQ(Selectivity("t.g = 1"), 0.25);         // 4 distinct
        EXPECT_DOUBLE_EQ(Selectivity("t.g <> 1"), 0.75);
        EXPECT_DOUBLE_EQ(Selectivity("t.v IS NULL"), 0.1);      // 10 of 100
        EXPECT_DOUBLE_EQ(Selectivity("t.v IS NOT NULL"), 0.9);
        EXPECT_DOUBLE_EQ(Selectivity("t.v = 50.0"), 0.9 / 90);  // non-NULL share / distinct
        EXPECT_NEAR(Selectivity("t.id > 75"), 25.0 / 99, 1e-9); // interpolated over 1..100
        EXPECT_NEAR(Selectivity("t.id <= 25"), 24.0 / 99, 1e-9);
        EXPECT_NEAR(Selectivity("t.id BETWEEN 10 AND 20"), 10.0 / 99, 1e-9);
        EXPECT_NEAR(Selectivity("t.id NOT BETWEEN 10 AND 20"), 1 - 10.0 / 99, 1e-9);
        EXPECT_DOUBLE_EQ(Selectivity("t.id > 1000"), 0.01);     // out of range, clamped
        EXPECT_DOUBLE_EQ(Selectivity("t.g IN (1, 2)"), 0.5);
        EXPECT_DOUBLE_EQ(Selectivity("t.s = 'x'"), 1.0);        // one value
    }

    TEST_F(EstimatorTest, JoinSizes)
    {
        auto stmt = Parse("SELECT * FROM t, u;");
        const Scope scope = Scope::ForSelect(*static_cast<const SelectStatement *>(stmt.get()), &catalog_);
        const Estimator estimator(scope, &catalog_);
        const Scope::Resolved t_id = scope.Resolve("t.id"), u_t_id = scope.Resolve("u.t_id");
        EXPECT_DOUBLE_EQ(estimator.Rows(0), 100);
        // Without statistics a column is assumed to be a key: max(|L|, |R|)
        EXPECT_DOUBLE_EQ(estimator.JoinRows(100, 3, t_id, u_t_id), 100 * 3 / 100.0);
        ASSERT_TRUE(catalog_.Analyze("u"));
        EXPECT_DOUBLE_EQ(estimator.JoinRows(100, 3, t_id, u_t_id), 3.0);          // distinct: 100 vs 2
        EXPECT_DOUBLE_EQ(estimator.Distinct(scope.Resolve("t.g")), 100);          // t not analyzed
        ASSERT_TRUE(catalog_.Analyze("t"));
        EXPECT_DOUBLE_EQ(estimator.Distinct(scope.Resolve("t.g")), 4);
    }

} // namespace sql
