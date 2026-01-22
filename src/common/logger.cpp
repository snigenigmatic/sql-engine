#include "common/logger.h"
#include <iostream>

namespace sql {

void Logger::Log(const std::string& message) {
    std::cout << "[LOG] " << message << std::endl;
}

} // namespace sql
