#pragma once

#include "storage/table.h"
#include <unordered_map>
#include <string>
#include <memory>

namespace sql {

class Catalog {
public:
    Catalog() = default;
    
    // Catalog methods (e.g., specific to Phase 1: in-memory)
    
private:
    std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
};

} // namespace sql
