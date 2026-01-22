#pragma once

#include "common/types.h"
#include <string>
#include <variant>

namespace sql {

class Value {
public:
    Value() = default;
    
    // Add constructors and methods here
    
private:
    DataType type_;
    // Internal storage
};

} // namespace sql
