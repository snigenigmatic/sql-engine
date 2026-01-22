#pragma once

#include <cstdint>
#include <array>

namespace sql {

// Page size constants
constexpr size_t PAGE_SIZE = 4096;

class Page {
public:
    Page() = default;
    
    // Page methods
    
private:
    std::array<char, PAGE_SIZE> data_;
};

} // namespace sql
