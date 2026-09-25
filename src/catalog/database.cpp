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
                *error = "Unable to open database: " + db->pager_->GetLastError();
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
            return nullptr; // destructor discards anything loading wrote
        }
        // Loading may have written (a new schema table, rebuilt indexes)
        if (!db->Commit())
        {
            if (error)
                *error = "Unable to write to '" + path + "'";
            return nullptr;
        }
        db->opened_ = true;
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
        db->opened_ = true;
        return db;
    }

    Database::~Database()
    {
        catalog_.reset();
        if (!bpm_)
            return;
        if (opened_)
        {
            Commit();
        }
        else
        {
            // Failed open: leave the file exactly as it was
            bpm_->DiscardAll();
            pager_->Rollback();
        }
    }

    bool Database::HasUncommittedChanges()
    {
        return bpm_->HasDirtyPages() || pager_->HasUncommittedChanges();
    }

    bool Database::Commit()
    {
        return bpm_->WriteDirtyPages() && pager_->Commit();
    }

    bool Database::Rollback(std::string *error)
    {
        auto fail = [&](const std::string &message)
        {
            if (error)
                *error = message;
            return false;
        };

        if (pager_->IsInMemory())
            return fail("rollback is not supported for in-memory databases");
        if (!bpm_->DiscardAll())
            return fail("cannot roll back while pages are in use");
        if (!pager_->Rollback())
            return fail("rollback failed: " + pager_->GetLastError());

        // Tables, indexes and cached counts are rebuilt from the rolled-back
        // pages
        catalog_.reset();
        try
        {
            catalog_ = std::make_unique<Catalog>(bpm_.get(), pager_.get());
        }
        catch (const std::exception &e)
        {
            return fail(std::string("cannot reload schema after rollback: ") + e.what());
        }
        return true;
    }

    bool Database::Checkpoint()
    {
        return Commit() && pager_->Checkpoint();
    }

} // namespace sql
