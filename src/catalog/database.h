#pragma once

#include "catalog/catalog.h"
#include "storage/buffer_pool.h"
#include "storage/pager.h"
#include <memory>
#include <string>

namespace sql
{

    // An open database: the file (Pager), its page cache (BufferPoolManager)
    // and the Catalog loaded from it.
    class Database
    {
    public:
        static constexpr size_t DEFAULT_POOL_SIZE = 1024; // 4 MB of pages

        // Open or create a database file. Returns nullptr and sets *error if
        // the file cannot be opened, is not a database, or has a corrupt schema.
        static std::unique_ptr<Database> Open(const std::string &path, std::string *error,
                                              size_t pool_size = DEFAULT_POOL_SIZE);

        static std::unique_ptr<Database> OpenInMemory(size_t pool_size = DEFAULT_POOL_SIZE);

        ~Database();

        Database(const Database &) = delete;
        Database &operator=(const Database &) = delete;

        Catalog &GetCatalog() { return *catalog_; }
        const std::string &GetPath() const { return path_; }

        // True if the database had no schema when opened (freshly created)
        bool WasCreated() const { return was_created_; }

        // Write all dirty pages and fsync
        bool Flush();

    private:
        Database() = default;

        std::string path_;
        bool was_created_ = false;

        // Destruction order matters: catalog, then buffer pool (flushes),
        // then pager (closes the file).
        std::unique_ptr<Pager> pager_;
        std::unique_ptr<BufferPoolManager> bpm_;
        std::unique_ptr<Catalog> catalog_;
    };

} // namespace sql
