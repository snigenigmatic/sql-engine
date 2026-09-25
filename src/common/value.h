#pragma once

#include "common/types.h"
#include <string>
#include <variant>
#include <iostream>

namespace sql
{

    class Value
    {
    public:
        Value() : type_(DataType::INTEGER), is_null_(true) {}
        Value(int32_t val) : type_(DataType::INTEGER), is_null_(false), value_(val) {}
        Value(double val) : type_(DataType::FLOAT), is_null_(false), value_(val) {}
        Value(bool val) : type_(DataType::BOOLEAN), is_null_(false), value_(val) {}
        Value(std::string val) : type_(DataType::VARCHAR), is_null_(false), value_(std::move(val)) {}
        Value(const char *val) : type_(DataType::VARCHAR), is_null_(false), value_(std::string(val)) {}
        // Null value constructor
        explicit Value(DataType type) : type_(type), is_null_(true) {}

        DataType GetType() const { return type_; }
        bool IsNull() const { return is_null_; }

        // Getters with type checking (throws if mismatch)
        int32_t GetAsInt() const;
        double GetAsFloat() const;
        bool GetAsBool() const;
        std::string GetAsString() const;

        // Comparison
        bool operator==(const Value &other) const;
        bool operator!=(const Value &other) const;
        bool operator<(const Value &other) const;
        bool operator>(const Value &other) const;
        bool operator<=(const Value &other) const;
        bool operator>=(const Value &other) const;

        // String representation
        std::string ToString() const;

        // Binary encoding: 1 tag byte (type, high bit = NULL) + fixed or
        // length-prefixed payload. Self-describing, so no schema is needed.
        void SerializeTo(std::string *out) const;
        // Decodes one value starting at *cursor and advances it. Returns false
        // on malformed or truncated input.
        static bool DeserializeFrom(const char **cursor, const char *end, Value *out);

    private:
        DataType type_;
        bool is_null_;
        std::variant<int32_t, double, bool, std::string> value_;
    };

} // namespace sql
