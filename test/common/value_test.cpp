#include <gtest/gtest.h>
#include "common/value.h"

namespace sql
{

    TEST(ValueTest, BasicAssertion)
    {
        // Test content
        EXPECT_EQ(1, 1);
    }

    TEST(ValueTest, FloatFormatting)
    {
        EXPECT_EQ(Value(0.1).ToString(), "0.1");
        EXPECT_EQ(Value(27.5).ToString(), "27.5");
        EXPECT_EQ(Value(2.0).ToString(), "2.0"); // stays visibly FLOAT
        EXPECT_EQ(Value(-3.0).ToString(), "-3.0");
        EXPECT_EQ(Value(-0.0).ToString(), "-0.0");
        EXPECT_EQ(Value(1e20).ToString(), "1e+20");
        EXPECT_EQ(Value(1.0 / 3.0).ToString(), "0.3333333333333333");
        EXPECT_EQ(Value(0.1 + 0.2).ToString(), "0.30000000000000004");
        EXPECT_EQ(FormatFloat(1.0 / 0.0), "inf");
        EXPECT_EQ(FormatFloat(-1.0 / 0.0), "-inf");
        // Always reads back as the same number
        for (double d : {0.1, 1.0 / 3.0, 123456.789, 1e-300, 6.02214076e23, -2.5e-7})
            EXPECT_EQ(std::stod(Value(d).ToString()), d) << Value(d).ToString();
        EXPECT_EQ(Value(7).ToString(), "7"); // INTEGER unchanged
    }

} // namespace sql
