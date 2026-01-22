#include "storage/disk_manager.h"

namespace sql {

DiskManager::DiskManager(const std::string& db_file) : file_name_(db_file) {}

} // namespace sql
