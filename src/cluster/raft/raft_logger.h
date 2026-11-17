#pragma once

#include "cluster/raft/nuraft.h"

namespace sql::cluster
{

    // Prints NuRaft's messages to stderr, up to a level:
    // 1 fatal, 2 error, 3 warning, 4 info, 5 debug, 6 trace
    class RaftLogger : public nuraft::logger
    {
    public:
        explicit RaftLogger(int level = 2) : level_(level) {}

        void set_level(int level) override { level_ = level; }
        int get_level() override { return level_; }
        void put_details(int level, const char *source_file, const char *func_name, size_t line_number,
                         const std::string &log_line) override;

    private:
        int level_;
    };

} // namespace sql::cluster
