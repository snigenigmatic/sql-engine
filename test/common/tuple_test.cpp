#include <gtest/gtest.h>
#include "common/tuple.h"
#include <string>

namespace sql
{

    TEST(TupleTest, BasicAssertion)
    {
        Tuple tuple({Value(1), Value("a")});
        EXPECT_EQ(tuple.GetValueCount(), 2u);
        EXPECT_EQ(tuple.ToString(), "(1, a)");
    }

    TEST(TupleTest, SerializationRoundTripsAllTypes)
    {
        Tuple original({Value(int32_t{-42}), Value(3.25), Value(true), Value("hello world"),
                        Value(std::string()), Value(DataType::VARCHAR), Value(DataType::FLOAT)});
        std::string bytes;
        original.SerializeTo(&bytes);

        Tuple decoded;
        ASSERT_TRUE(Tuple::DeserializeFrom(bytes.data(), bytes.size(), &decoded));
        ASSERT_EQ(decoded.GetValueCount(), original.GetValueCount());
        for (size_t i = 0; i < original.GetValueCount(); ++i)
        {
            EXPECT_EQ(decoded.GetValue(i).GetType(), original.GetValue(i).GetType()) << i;
            EXPECT_EQ(decoded.GetValue(i).IsNull(), original.GetValue(i).IsNull()) << i;
            EXPECT_EQ(decoded.GetValue(i), original.GetValue(i)) << i;
        }
    }

    TEST(TupleTest, DeserializeRejectsTruncatedInput)
    {
        Tuple original({Value(7), Value("abcdef")});
        std::string bytes;
        original.SerializeTo(&bytes);

        Tuple decoded;
        for (size_t len = 0; len < bytes.size(); ++len)
            EXPECT_FALSE(Tuple::DeserializeFrom(bytes.data(), len, &decoded)) << len;
    }

    TEST(TupleTest, DeserializeRejectsTrailingBytes)
    {
        std::string bytes;
        Tuple({Value(1)}).SerializeTo(&bytes);
        bytes.push_back('x');
        Tuple decoded;
        EXPECT_FALSE(Tuple::DeserializeFrom(bytes.data(), bytes.size(), &decoded));
    }

    TEST(TupleTest, DeserializeRejectsInvalidBooleanByte)
    {
        std::string bytes;
        Tuple({Value(true)}).SerializeTo(&bytes);
        ASSERT_EQ(bytes.back(), 1);
        bytes.back() = 2;
        Tuple decoded;
        EXPECT_FALSE(Tuple::DeserializeFrom(bytes.data(), bytes.size(), &decoded));
    }

} // namespace sql
