#pragma once

#include <string>

namespace sql {

class Logger {
public:
    static void Log(const std::string& message);
};

} // namespace sql
