#include "cluster/raft/raft_logger.h"
#include <cstring>
#include <iostream>

namespace sql::cluster
{

    void RaftLogger::put_details(int level, const char *source_file, const char * /*func_name*/,
                                 size_t line_number, const std::string &log_line)
    {
        if (level > level_)
            return;
        static const char *const names[] = {"", "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
        const char *file = std::strrchr(source_file, '/');
        std::cerr << "[raft " << (level >= 1 && level <= 6 ? names[level] : "?") << "] "
                  << (file ? file + 1 : source_file) << ":" << line_number << " " << log_line << "\n";
    }

} // namespace sql::cluster
