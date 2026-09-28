#pragma once

#include <cstdint>

namespace sql
{

    enum class DataType
    {
        INTEGER,
        FLOAT,
        VARCHAR,
        BOOLEAN
    };

    inline const char *DataTypeName(DataType type)
    {
        switch (type)
        {
        case DataType::INTEGER:
            return "INTEGER";
        case DataType::FLOAT:
            return "FLOAT";
        case DataType::VARCHAR:
            return "VARCHAR";
        case DataType::BOOLEAN:
            return "BOOLEAN";
        }
        return "UNKNOWN";
    }

} // namespace sql
