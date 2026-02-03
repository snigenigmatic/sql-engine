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

    private:
        DataType type_;
        bool is_null_;
        std::variant<int32_t, double, bool, std::string> value_;
    };

} // namespace sql
