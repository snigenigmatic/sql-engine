#pragma once

#include "storage/page.h"
#include "storage/disk_manager.h"

namespace sql
{

    class BufferPoolManager
    {
    public:
        explicit BufferPoolManager(DiskManager *disk_manager);

        // Buffer pool methods

    private:
        DiskManager *disk_manager_;
    };

} // namespace sql
