#pragma once

#include <string>

namespace sql
{

    class DiskManager
    {
    public:
        explicit DiskManager(const std::string &db_file);

        // Disk manager methods

    private:
        std::string file_name_;
    };

} // namespace sql
