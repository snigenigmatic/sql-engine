#pragma once

#include "common/value.h"
#include "common/schema.h"
#include <vector>

namespace sql {

class Tuple {
public:
    Tuple() = default;
    
    // Tuple methods
    
private:
    std::vector<Value> values_;
};

} // namespace sql
