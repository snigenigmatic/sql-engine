#pragma once

#include "storage/page.h"
#include "storage/wal.h"
#include <array>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace sql
{

    // Page I/O on a single database file, made atomic and durable by a
    // write-ahead log ("<path>-wal").
    //
    // Page 0 is the file header:
    //   [0..8)   magic "SQLENG01"
    //   [8..12)  format version
    //   [12..16) page count (including the header page)
    //   [16..20) free-list head page id (INVALID_PAGE_ID if empty)
    //   [20..24) catalog root page id   (INVALID_PAGE_ID until set)
    //
    // Freed pages form a singly linked list: the first 4 bytes of a free page
    // hold the id of the next free page.
    //
    // Writes go to the log, not the database file. Commit() appends the
    // header page as a commit frame and fsyncs, making every change since
    // the previous commit durable at once; Rollback() discards them. The
    // database file is only written by checkpoints (when the log grows past a
    // threshold, and on close), and after a crash Open() replays the
    // committed part of the log, so the file always reflects a whole number
    // of committed transactions.
    class Pager
    {
    public:
        static constexpr uint32_t FORMAT_VERSION = 1;
        static constexpr uint64_t DEFAULT_CHECKPOINT_FRAMES = 1000; // ~4 MB of log

        Pager() = default;
        ~Pager();

        Pager(const Pager &) = delete;
        Pager &operator=(const Pager &) = delete;

        // Open (or create) the database file, recovering committed work from
        // its log. Returns false (see GetLastError) on I/O error, if the file
        // is not a valid database, or if another connection has it open.
        bool Open(const std::string &path);

        // Open a transient database held entirely in memory (like SQLite's
        // ":memory:"). Nothing is written to disk; Commit is a no-op and
        // Rollback is not supported.
        void OpenInMemory();

        // Commits outstanding changes, checkpoints, and removes the log.
        void Close();
        bool IsOpen() const { return fd_ >= 0 || in_memory_; }
        bool IsInMemory() const { return in_memory_; }
        const std::string &GetLastError() const { return last_error_; }

        bool ReadPage(page_id_t page_id, char *out);
        bool WritePage(page_id_t page_id, const char *data);

        // Returns a zeroed page from the free list, or extends the database.
        page_id_t AllocatePage();

        // Push a page onto the free list. Caller must ensure the page is not
        // cached in a buffer pool.
        bool DeallocatePage(page_id_t page_id);

        // Make every change since the last commit durable, atomically
        bool Commit();

        // Discard every change since the last commit (file databases only)
        bool Rollback();

    private:
        struct HeaderState
        {
            uint32_t page_count = 0;
            page_id_t free_list_head = INVALID_PAGE_ID;
            page_id_t catalog_root = INVALID_PAGE_ID;
        };

    public:
        // A point inside the open transaction to roll back to. Taken after
        // the buffer pool has written its dirty pages, so it captures the
        // whole state at that moment.
        struct Savepoint
        {
            WriteAheadLog::Savepoint wal;
            HeaderState header;
            bool header_dirty = false;
            std::unordered_set<page_id_t> fresh_pages;
        };
        Savepoint CreateSavepoint() const;

        // Discard every change made after the savepoint (file databases only)
        bool RollbackTo(const Savepoint &savepoint);

        // Copy committed pages from the log into the database file and empty
        // the log. Fails if there are uncommitted changes.
        bool Checkpoint();

        bool HasUncommittedChanges() const;
        uint64_t GetWalFrameCount() const { return wal_.GetFrameCount(); }
        void SetCheckpointThreshold(uint64_t frames) { checkpoint_threshold_ = frames; }

        bool Sync();

        uint32_t GetPageCount() const { return page_count_; }
        page_id_t GetFreeListHead() const { return free_list_head_; }
        page_id_t GetCatalogRoot() const { return catalog_root_; }
        bool SetCatalogRoot(page_id_t page_id);

    private:
        bool Fail(const std::string &message);
        void BuildHeader(char *buf) const;
        bool WriteHeader();
        bool ReadHeader();
        bool IsValidHeaderPageId(page_id_t page_id) const;
        HeaderState CurrentHeader() const { return {page_count_, free_list_head_, catalog_root_}; }

        // Copy the log's committed pages into the database file, then reset it
        bool CheckpointLog();

        // Raw page access for either backend (page 0 included)
        bool RawRead(page_id_t page_id, char *out);
        bool RawWrite(page_id_t page_id, const char *data);

        int fd_ = -1;
        bool in_memory_ = false;
        std::vector<std::unique_ptr<std::array<char, PAGE_SIZE>>> mem_pages_;
        std::string last_error_;

        WriteAheadLog wal_;
        uint64_t checkpoint_threshold_ = DEFAULT_CHECKPOINT_FRAMES;

        uint32_t page_count_ = 0;
        page_id_t free_list_head_ = INVALID_PAGE_ID;
        page_id_t catalog_root_ = INVALID_PAGE_ID;

        // State as of the last commit, restored by Rollback
        HeaderState committed_header_;
        bool header_dirty_ = false;

        // Pages allocated since the last commit and not written yet: their
        // content is all zeros
        std::unordered_set<page_id_t> fresh_pages_;
    };

} // namespace sql
