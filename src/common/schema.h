#pragma once

#include "common/types.h"
#include <vector>
#include <string>

namespace sql {

struct ColumnDefinition {
    std::string name;
    DataType type;
};

class Schema {
public:
    Schema() = default;
    
    // Schema methods
    
private:
    std::vector<ColumnDefinition> columns_;
};

} // namespace sql
