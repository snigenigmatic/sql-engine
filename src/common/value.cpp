#include "common/value.h"
#include <cstring>
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

    namespace
    {
        constexpr uint8_t NULL_FLAG = 0x80;

        template <typename T>
        void AppendRaw(std::string *out, const T &v)
        {
            out->append(reinterpret_cast<const char *>(&v), sizeof(T));
        }

        template <typename T>
        bool ReadRaw(const char **cursor, const char *end, T *v)
        {
            if (end - *cursor < static_cast<std::ptrdiff_t>(sizeof(T)))
                return false;
            std::memcpy(v, *cursor, sizeof(T));
            *cursor += sizeof(T);
            return true;
        }
    } // namespace

    void Value::SerializeTo(std::string *out) const
    {
        uint8_t tag = static_cast<uint8_t>(type_);
        if (is_null_)
        {
            out->push_back(static_cast<char>(tag | NULL_FLAG));
            return;
        }
        out->push_back(static_cast<char>(tag));
        switch (type_)
        {
        case DataType::INTEGER:
            AppendRaw(out, std::get<int32_t>(value_));
            break;
        case DataType::FLOAT:
            AppendRaw(out, std::get<double>(value_));
            break;
        case DataType::BOOLEAN:
            out->push_back(std::get<bool>(value_) ? 1 : 0);
            break;
        case DataType::VARCHAR:
        {
            const auto &s = std::get<std::string>(value_);
            AppendRaw(out, static_cast<uint32_t>(s.size()));
            out->append(s);
            break;
        }
        }
    }

    bool Value::DeserializeFrom(const char **cursor, const char *end, Value *out)
    {
        uint8_t tag;
        if (!ReadRaw(cursor, end, &tag))
            return false;

        const bool is_null = (tag & NULL_FLAG) != 0;
        const uint8_t type_bits = tag & static_cast<uint8_t>(~NULL_FLAG);
        if (type_bits > static_cast<uint8_t>(DataType::BOOLEAN))
            return false;
        const DataType type = static_cast<DataType>(type_bits);

        if (is_null)
        {
            *out = Value(type);
            return true;
        }

        switch (type)
        {
        case DataType::INTEGER:
        {
            int32_t v;
            if (!ReadRaw(cursor, end, &v))
                return false;
            *out = Value(v);
            return true;
        }
        case DataType::FLOAT:
        {
            double v;
            if (!ReadRaw(cursor, end, &v))
                return false;
            *out = Value(v);
            return true;
        }
        case DataType::BOOLEAN:
        {
            uint8_t v;
            if (!ReadRaw(cursor, end, &v) || v > 1)
                return false;
            *out = Value(v != 0);
            return true;
        }
        case DataType::VARCHAR:
        {
            uint32_t len;
            if (!ReadRaw(cursor, end, &len) || end - *cursor < static_cast<std::ptrdiff_t>(len))
                return false;
            *out = Value(std::string(*cursor, len));
            *cursor += len;
            return true;
        }
        }
        return false;
    }

} // namespace sql
