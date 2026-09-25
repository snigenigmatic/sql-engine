#pragma once

#include "catalog/catalog.h"
#include "storage/buffer_pool.h"
#include "storage/pager.h"
#include <memory>
#include <string>

namespace sql
{

    // An open database: the file (Pager, with its write-ahead log), its page
    // cache (BufferPoolManager) and the Catalog loaded from it.
    //
    // Changes become durable, atomically, at Commit(); Rollback() discards
    // everything since the last commit. The REPL commits after every
    // successful statement and rolls back a failed one, so each statement is
    // atomic. Closing a database commits outstanding changes.
    class Database
    {
    public:
        static constexpr size_t DEFAULT_POOL_SIZE = 1024; // 4 MB of pages

        // Open or create a database file, recovering committed work from its
        // write-ahead log after a crash. Returns nullptr and sets *error if
        // the file cannot be opened or locked, is not a database, or has a
        // corrupt schema.
        static std::unique_ptr<Database> Open(const std::string &path, std::string *error,
                                              size_t pool_size = DEFAULT_POOL_SIZE);

        static std::unique_ptr<Database> OpenInMemory(size_t pool_size = DEFAULT_POOL_SIZE);

        ~Database();

        Database(const Database &) = delete;
        Database &operator=(const Database &) = delete;

        // Replaced by Rollback(): do not hold on to it across a rollback
        Catalog &GetCatalog() { return *catalog_; }
        const std::string &GetPath() const { return path_; }
        Pager &GetPager() { return *pager_; }

        // True if the database had no schema when opened (freshly created)
        bool WasCreated() const { return was_created_; }

        // Anything changed since the last commit (cached or in the log)
        bool HasUncommittedChanges();

        // Make all changes since the last commit durable, atomically
        bool Commit();

        // Discard all changes since the last commit and reload the catalog.
        // Not supported for in-memory databases.
        bool Rollback(std::string *error = nullptr);

        // Commit, then copy the log into the database file
        bool Checkpoint();

        // Same as Commit()
        bool Flush() { return Commit(); }

    private:
        Database() = default;

        std::string path_;
        bool was_created_ = false;
        bool opened_ = false;

        // Destruction order matters: catalog, then buffer pool, then pager
        // (which commits and closes the file).
        std::unique_ptr<Pager> pager_;
        std::unique_ptr<BufferPoolManager> bpm_;
        std::unique_ptr<Catalog> catalog_;
    };

} // namespace sql
