#include "catalog/database.h"
#include <stdexcept>

namespace sql
{

    std::unique_ptr<Database> Database::Open(const std::string &path, std::string *error, size_t pool_size)
    {
        std::unique_ptr<Database> db(new Database());
        db->path_ = path;
        db->pager_ = std::make_unique<Pager>();
        if (!db->pager_->Open(path))
        {
            if (error)
                *error = "Unable to open '" + path + "': not a database file or not accessible";
            return nullptr;
        }
        db->was_created_ = db->pager_->GetCatalogRoot() == INVALID_PAGE_ID;
        db->bpm_ = std::make_unique<BufferPoolManager>(pool_size, db->pager_.get());
        try
        {
            db->catalog_ = std::make_unique<Catalog>(db->bpm_.get(), db->pager_.get());
        }
        catch (const std::exception &e)
        {
            if (error)
                *error = "Unable to load schema from '" + path + "': " + e.what();
            return nullptr;
        }
        return db;
    }

    std::unique_ptr<Database> Database::OpenInMemory(size_t pool_size)
    {
        std::unique_ptr<Database> db(new Database());
        db->path_ = ":memory:";
        db->was_created_ = true;
        db->pager_ = std::make_unique<Pager>();
        db->pager_->OpenInMemory();
        db->bpm_ = std::make_unique<BufferPoolManager>(pool_size, db->pager_.get());
        db->catalog_ = std::make_unique<Catalog>(db->bpm_.get(), db->pager_.get());
        return db;
    }

    Database::~Database()
    {
        catalog_.reset();
        if (bpm_)
            bpm_->FlushAll();
    }

    bool Database::Flush()
    {
        return bpm_->FlushAll();
    }

} // namespace sql
