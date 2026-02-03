#include "common/value.h"

#include "common/value.h"
#include <stdexcept>
#include <sstream>

namespace sql
{

    int32_t Value::GetAsInt() const
    {
        if (type_ != DataType::INTEGER)
            throw std::runtime_error("Type mismatch: Expected INTEGER");
        return std::get<int32_t>(value_);
    }

    double Value::GetAsFloat() const
    {
        if (type_ != DataType::FLOAT)
            throw std::runtime_error("Type mismatch: Expected FLOAT");
        return std::get<double>(value_);
    }

    bool Value::GetAsBool() const
    {
        if (type_ != DataType::BOOLEAN)
            throw std::runtime_error("Type mismatch: Expected BOOLEAN");
        return std::get<bool>(value_);
    }

    std::string Value::GetAsString() const
    {
        if (type_ != DataType::VARCHAR)
            throw std::runtime_error("Type mismatch: Expected VARCHAR");
        return std::get<std::string>(value_);
    }

    bool Value::operator==(const Value &other) const
    {
        if (type_ != other.type_)
            return false;
        if (is_null_ && other.is_null_)
            return true;
        if (is_null_ || other.is_null_)
            return false;
        return value_ == other.value_;
    }

    bool Value::operator!=(const Value &other) const
    {
        return !(*this == other);
    }

    bool Value::operator<(const Value &other) const
    {
        if (is_null_ || other.is_null_)
            return false;
        if (type_ != other.type_)
            throw std::runtime_error("Cannot compare different types");
        return value_ < other.value_;
    }

    bool Value::operator>(const Value &other) const
    {
        return other < *this;
    }

    bool Value::operator<=(const Value &other) const
    {
        return !(*this > other);
    }

    bool Value::operator>=(const Value &other) const
    {
        return !(*this < other);
    }

    std::string Value::ToString() const
    {
        if (is_null_)
            return "NULL";
        switch (type_)
        {
        case DataType::INTEGER:
            return std::to_string(std::get<int32_t>(value_));
        case DataType::FLOAT:
            return std::to_string(std::get<double>(value_));
        case DataType::BOOLEAN:
            return std::get<bool>(value_) ? "true" : "false";
        case DataType::VARCHAR:
            return std::get<std::string>(value_);
        default:
            return "";
        }
    }

} // namespace sql
