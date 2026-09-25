#include "storage/pager.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <random>

namespace sql
{

    namespace
    {
        constexpr char MAGIC[8] = {'S', 'Q', 'L', 'E', 'N', 'G', '0', '1'};
        constexpr size_t OFF_VERSION = 8;
        constexpr size_t OFF_PAGE_COUNT = 12;
        constexpr size_t OFF_FREE_HEAD = 16;
        constexpr size_t OFF_CATALOG_ROOT = 20;
        constexpr size_t OFF_DB_ID = 24;

        uint64_t NewDatabaseId()
        {
            std::random_device rd;
            uint64_t id = 0;
            while (id == 0)
                id = (static_cast<uint64_t>(rd()) << 32) | rd();
            return id;
        }

        bool PreadFull(int fd, char *buf, size_t len, off_t offset)
        {
            size_t done = 0;
            while (done < len)
            {
                ssize_t n = pread(fd, buf + done, len - done, offset + static_cast<off_t>(done));
                if (n <= 0)
                    return false;
                done += static_cast<size_t>(n);
            }
            return true;
        }

        bool PwriteFull(int fd, const char *buf, size_t len, off_t offset)
        {
            size_t done = 0;
            while (done < len)
            {
                ssize_t n = pwrite(fd, buf + done, len - done, offset + static_cast<off_t>(done));
                if (n <= 0)
                    return false;
                done += static_cast<size_t>(n);
            }
            return true;
        }

        off_t PageOffset(page_id_t page_id)
        {
            return static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
        }

        bool FileExists(const std::string &path)
        {
            struct stat st;
            return stat(path.c_str(), &st) == 0;
        }
    } // namespace

    Pager::~Pager()
    {
        Close();
    }

    bool Pager::Fail(const std::string &message)
    {
        last_error_ = message;
        wal_.Close(false);
        if (fd_ >= 0)
        {
            close(fd_);
            fd_ = -1;
        }
        return false;
    }

    bool Pager::Open(const std::string &path)
    {
        Close();
        last_error_.clear();

        fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ < 0)
            return Fail("cannot open '" + path + "'");
        // One connection at a time: two writers would each think they own
        // the log
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0)
            return Fail("database '" + path + "' is locked by another connection");

        struct stat st;
        if (fstat(fd_, &st) != 0)
            return Fail("cannot stat '" + path + "'");

        const std::string wal_path = path + "-wal";
        if (st.st_size == 0)
        {
            // New database. A log left behind by an earlier database with
            // the same name must not be used with it.
            if (unlink(wal_path.c_str()) != 0 && errno != ENOENT)
                return Fail("cannot remove stale write-ahead log '" + wal_path + "'");
            page_count_ = 1;
            free_list_head_ = INVALID_PAGE_ID;
            catalog_root_ = INVALID_PAGE_ID;
            db_id_ = NewDatabaseId();
            std::array<char, PAGE_SIZE> buf{};
            BuildHeader(buf.data());
            if (!RawWrite(0, buf.data()) || fsync(fd_) != 0)
                return Fail("cannot initialize '" + path + "'");
        }
        else
        {
            // Reject foreign files before touching any log
            std::array<char, PAGE_SIZE> page0{};
            if (!PreadFull(fd_, page0.data(), sizeof(MAGIC), 0) ||
                std::memcmp(page0.data(), MAGIC, sizeof(MAGIC)) != 0)
                return Fail("'" + path + "' is not a database file");

            // The id is fixed at creation, so the file's copy is authoritative
            if (!RawRead(0, page0.data()))
                return Fail("'" + path + "' has a corrupt header");
            std::memcpy(&db_id_, page0.data() + OFF_DB_ID, sizeof(db_id_));

            // Recover transactions committed before a crash. They stay in the
            // log (reads go through it) until the next checkpoint.
            if (FileExists(wal_path))
            {
                std::string error;
                if (!wal_.Open(wal_path, db_id_, &error))
                    return Fail(error);
            }

            if (!ReadHeader() || !IsValidHeaderPageId(free_list_head_) || !IsValidHeaderPageId(catalog_root_))
                return Fail("'" + path + "' has a corrupt header");
            // Every declared page must exist (pages newer than the last
            // checkpoint live in the log)
            if (wal_.GetCommittedFrameCount() == 0 &&
                static_cast<uint64_t>(st.st_size) < static_cast<uint64_t>(page_count_) * PAGE_SIZE)
                return Fail("'" + path + "' has a corrupt header");

            if (db_id_ == 0)
            {
                // Written before database ids existed (and so before logs):
                // give it one now, directly in the file
                db_id_ = NewDatabaseId();
                std::array<char, PAGE_SIZE> buf{};
                BuildHeader(buf.data());
                if (!RawWrite(0, buf.data()) || fsync(fd_) != 0)
                    return Fail("cannot update '" + path + "'");
            }
        }

        if (!wal_.IsOpen())
        {
            std::string error;
            if (!wal_.Open(wal_path, db_id_, &error))
                return Fail(error);
        }

        committed_header_ = CurrentHeader();
        header_dirty_ = false;
        fresh_pages_.clear();
        return true;
    }

    void Pager::OpenInMemory()
    {
        Close();
        in_memory_ = true;
        page_count_ = 1;
        free_list_head_ = INVALID_PAGE_ID;
        catalog_root_ = INVALID_PAGE_ID;
        mem_pages_.clear();
        mem_pages_.push_back(std::make_unique<std::array<char, PAGE_SIZE>>());
        WriteHeader();
    }

    void Pager::Close()
    {
        if (in_memory_)
        {
            in_memory_ = false;
            mem_pages_.clear();
        }
        if (fd_ >= 0)
        {
            // Outstanding writes are committed on close. Keep the log if
            // anything fails so recovery can finish the job.
            const bool clean = Commit() && Checkpoint();
            wal_.Close(clean);
            fsync(fd_);
            close(fd_); // also releases the lock
            fd_ = -1;
        }
        fresh_pages_.clear();
        header_dirty_ = false;
    }

    void Pager::Abandon()
    {
        wal_.Close(false);
        if (fd_ >= 0)
        {
            close(fd_); // also releases the lock
            fd_ = -1;
        }
        fresh_pages_.clear();
        header_dirty_ = false;
    }

    bool Pager::RawRead(page_id_t page_id, char *out)
    {
        if (in_memory_)
        {
            std::memcpy(out, mem_pages_[static_cast<size_t>(page_id)]->data(), PAGE_SIZE);
            return true;
        }
        return PreadFull(fd_, out, PAGE_SIZE, PageOffset(page_id));
    }

    bool Pager::RawWrite(page_id_t page_id, const char *data)
    {
        if (in_memory_)
        {
            while (mem_pages_.size() <= static_cast<size_t>(page_id))
                mem_pages_.push_back(std::make_unique<std::array<char, PAGE_SIZE>>());
            std::memcpy(mem_pages_[static_cast<size_t>(page_id)]->data(), data, PAGE_SIZE);
            return true;
        }
        return PwriteFull(fd_, data, PAGE_SIZE, PageOffset(page_id));
    }

    bool Pager::ReadHeader()
    {
        std::array<char, PAGE_SIZE> buf{};
        if (!wal_.ReadPage(0, buf.data()) && !RawRead(0, buf.data()))
            return false;
        if (std::memcmp(buf.data(), MAGIC, sizeof(MAGIC)) != 0)
            return false;

        uint32_t version;
        std::memcpy(&version, buf.data() + OFF_VERSION, sizeof(version));
        if (version != FORMAT_VERSION)
            return false;

        std::memcpy(&page_count_, buf.data() + OFF_PAGE_COUNT, sizeof(page_count_));
        std::memcpy(&free_list_head_, buf.data() + OFF_FREE_HEAD, sizeof(free_list_head_));
        std::memcpy(&catalog_root_, buf.data() + OFF_CATALOG_ROOT, sizeof(catalog_root_));
        return page_count_ >= 1;
    }

    bool Pager::IsValidHeaderPageId(page_id_t page_id) const
    {
        return page_id == INVALID_PAGE_ID || (page_id >= 1 && static_cast<uint32_t>(page_id) < page_count_);
    }

    void Pager::BuildHeader(char *buf) const
    {
        std::memset(buf, 0, PAGE_SIZE);
        std::memcpy(buf, MAGIC, sizeof(MAGIC));
        uint32_t version = FORMAT_VERSION;
        std::memcpy(buf + OFF_VERSION, &version, sizeof(version));
        std::memcpy(buf + OFF_PAGE_COUNT, &page_count_, sizeof(page_count_));
        std::memcpy(buf + OFF_FREE_HEAD, &free_list_head_, sizeof(free_list_head_));
        std::memcpy(buf + OFF_CATALOG_ROOT, &catalog_root_, sizeof(catalog_root_));
        std::memcpy(buf + OFF_DB_ID, &db_id_, sizeof(db_id_));
    }

    bool Pager::WriteHeader()
    {
        if (!in_memory_)
        {
            // Written to the log as the next commit frame
            header_dirty_ = true;
            return true;
        }
        std::array<char, PAGE_SIZE> buf{};
        BuildHeader(buf.data());
        return RawWrite(0, buf.data());
    }

    bool Pager::ReadPage(page_id_t page_id, char *out)
    {
        if (!IsOpen() || page_id <= 0 || static_cast<uint32_t>(page_id) >= page_count_)
            return false;
        if (in_memory_)
            return RawRead(page_id, out);
        if (fresh_pages_.count(page_id))
        {
            std::memset(out, 0, PAGE_SIZE);
            return true;
        }
        if (wal_.ReadPage(page_id, out))
            return true;
        return RawRead(page_id, out);
    }

    bool Pager::WritePage(page_id_t page_id, const char *data)
    {
        if (!IsOpen() || page_id <= 0 || static_cast<uint32_t>(page_id) >= page_count_)
            return false;
        if (in_memory_)
            return RawWrite(page_id, data);
        fresh_pages_.erase(page_id);
        return wal_.AppendFrame(page_id, data, 0);
    }

    page_id_t Pager::AllocatePage()
    {
        if (!IsOpen())
            return INVALID_PAGE_ID;

        page_id_t page_id;
        if (free_list_head_ != INVALID_PAGE_ID)
        {
            page_id = free_list_head_;
            std::array<char, PAGE_SIZE> buf{};
            if (!ReadPage(page_id, buf.data()))
                return INVALID_PAGE_ID;
            page_id_t next;
            std::memcpy(&next, buf.data(), sizeof(next));
            if (!IsValidHeaderPageId(next))
                return INVALID_PAGE_ID; // corrupt free list
            free_list_head_ = next;
        }
        else
        {
            page_id = static_cast<page_id_t>(page_count_);
            page_count_++;
        }

        if (in_memory_)
        {
            std::array<char, PAGE_SIZE> zeros{};
            RawWrite(page_id, zeros.data());
        }
        else
        {
            fresh_pages_.insert(page_id);
        }
        WriteHeader();
        return page_id;
    }

    bool Pager::DeallocatePage(page_id_t page_id)
    {
        if (!IsOpen() || page_id <= 0 || static_cast<uint32_t>(page_id) >= page_count_)
            return false;

        std::array<char, PAGE_SIZE> buf{};
        std::memcpy(buf.data(), &free_list_head_, sizeof(free_list_head_));
        if (!WritePage(page_id, buf.data()))
            return false;
        free_list_head_ = page_id;
        return WriteHeader();
    }

    bool Pager::SetCatalogRoot(page_id_t page_id)
    {
        catalog_root_ = page_id;
        return WriteHeader();
    }

    bool Pager::HasUncommittedChanges() const
    {
        return !in_memory_ && fd_ >= 0 &&
               (wal_.HasUncommittedFrames() || header_dirty_ || !fresh_pages_.empty());
    }

    bool Pager::Commit()
    {
        if (!HasUncommittedChanges())
            return true;

        // Pages allocated but never written still have to exist
        std::array<char, PAGE_SIZE> buf{};
        for (page_id_t page_id : fresh_pages_)
        {
            if (!wal_.AppendFrame(page_id, buf.data(), 0))
                return false;
        }
        fresh_pages_.clear();

        // The header page is the commit frame
        BuildHeader(buf.data());
        if (!wal_.AppendFrame(0, buf.data(), page_count_))
            return false;
        committed_header_ = CurrentHeader();
        header_dirty_ = false;

        if (wal_.GetFrameCount() >= checkpoint_threshold_)
            Checkpoint(); // best effort; the commit is already durable
        return true;
    }

    bool Pager::Rollback()
    {
        if (in_memory_)
        {
            last_error_ = "rollback is not supported for in-memory databases";
            return false;
        }
        if (fd_ < 0)
            return false;
        fresh_pages_.clear();
        header_dirty_ = false;
        page_count_ = committed_header_.page_count;
        free_list_head_ = committed_header_.free_list_head;
        catalog_root_ = committed_header_.catalog_root;
        return wal_.Rollback();
    }

    Pager::Savepoint Pager::CreateSavepoint() const
    {
        return {wal_.CreateSavepoint(), CurrentHeader(), header_dirty_, fresh_pages_};
    }

    bool Pager::RollbackTo(const Savepoint &savepoint)
    {
        if (in_memory_)
        {
            last_error_ = "rollback is not supported for in-memory databases";
            return false;
        }
        if (fd_ < 0 || !wal_.RollbackTo(savepoint.wal))
        {
            last_error_ = "cannot roll back to savepoint";
            return false;
        }
        page_count_ = savepoint.header.page_count;
        free_list_head_ = savepoint.header.free_list_head;
        catalog_root_ = savepoint.header.catalog_root;
        header_dirty_ = savepoint.header_dirty;
        fresh_pages_ = savepoint.fresh_pages;
        return true;
    }

    bool Pager::CheckpointLog()
    {
        std::array<char, PAGE_SIZE> buf{};
        for (const auto &entry : wal_.GetCommittedPages())
        {
            if (!wal_.ReadFrameData(entry.second, buf.data()) || !RawWrite(entry.first, buf.data()))
                return false;
        }

        // Every page counted by the last commit must exist in the file
        const uint64_t min_size = static_cast<uint64_t>(wal_.GetCommittedPageCount()) * PAGE_SIZE;
        struct stat st;
        if (fstat(fd_, &st) != 0)
            return false;
        if (static_cast<uint64_t>(st.st_size) < min_size && ftruncate(fd_, static_cast<off_t>(min_size)) != 0)
            return false;

        // The database file must be durable before the log is emptied
        if (fsync(fd_) != 0)
            return false;
        return wal_.Reset();
    }

    bool Pager::Checkpoint()
    {
        if (in_memory_)
            return true;
        if (fd_ < 0)
            return false;
        if (HasUncommittedChanges())
        {
            last_error_ = "cannot checkpoint with uncommitted changes";
            return false;
        }
        if (wal_.GetCommittedFrameCount() == 0)
            return true;
        return CheckpointLog();
    }

    bool Pager::Sync()
    {
        if (in_memory_)
            return true;
        return wal_.Sync();
    }

} // namespace sql
