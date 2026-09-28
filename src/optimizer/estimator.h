#pragma once

#include "optimizer/optimizer.h"

namespace sql
{

    // Cardinality estimates for the planner. Row counts are the tables'
    // live counts; column statistics come from the last ANALYZE, and without
    // them fixed defaults stand in (chosen so that, unanalyzed, an index is
    // used wherever one applies).
    class Estimator
    {
    public:
        // Selectivities without statistics
        static constexpr double DEFAULT_EQUALITY = 0.1;
        static constexpr double DEFAULT_RANGE = 0.2;
        static constexpr double DEFAULT_IS_NULL = 0.1;
        static constexpr double DEFAULT_LIKE = 0.25;
        static constexpr double DEFAULT_OTHER = 0.33;
        // An index scan is worth it when it keeps at most this share of rows
        static constexpr double INDEX_THRESHOLD = 0.25;

        Estimator(const Scope &scope, Catalog *catalog) : scope_(scope), catalog_(catalog) {}

        double Rows(size_t relation) const;
        // Distinct non-NULL values of a column (defaults to the row count)
        double Distinct(const Scope::Resolved &column) const;
        // Fraction of rows for which the condition is TRUE
        double Selectivity(const Expression *condition) const;
        // Rows of an equi-join of inputs of the given sizes on these columns
        double JoinRows(double left_rows, double right_rows, const Scope::Resolved &left,
                        const Scope::Resolved &right) const;

    private:
        const ColumnStats *Stats(const Scope::Resolved &column) const;
        // Whether a unique index covers the column
        bool Unique(const Scope::Resolved &column) const;
        // col = literal / col < literal ... with the column on either side
        double Comparison(TokenType op, const Scope::Resolved &column, const Value &literal) const;
        // Share of the column's values in [low, high] (either end open)
        std::optional<double> RangeFraction(const Scope::Resolved &column, const std::optional<Value> &low,
                                            const std::optional<Value> &high) const;
        double NullFraction(const Scope::Resolved &column) const;
        std::optional<Scope::Resolved> Column(const Expression *expr) const;

        const Scope &scope_;
        Catalog *catalog_;
    };

} // namespace sql
