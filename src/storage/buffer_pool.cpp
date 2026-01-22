#include "storage/buffer_pool.h"

namespace sql {

BufferPoolManager::BufferPoolManager(DiskManager* disk_manager) 
    : disk_manager_(disk_manager) {}

} // namespace sql
