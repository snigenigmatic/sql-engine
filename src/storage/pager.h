#pragma once

#include "storage/page.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace sql
{

    // Raw page I/O on a single database file.
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
    class Pager
    {
    public:
        static constexpr uint32_t FORMAT_VERSION = 1;

        Pager() = default;
        ~Pager();

        Pager(const Pager &) = delete;
        Pager &operator=(const Pager &) = delete;

        // Open (or create) the database file. Returns false on I/O error or
        // if an existing file is not a valid database.
        bool Open(const std::string &path);

        // Open a transient database held entirely in memory (like SQLite's
        // ":memory:"). Nothing is written to disk.
        void OpenInMemory();

        void Close();
        bool IsOpen() const { return fd_ >= 0 || in_memory_; }
        bool IsInMemory() const { return in_memory_; }

        bool ReadPage(page_id_t page_id, char *out);
        bool WritePage(page_id_t page_id, const char *data);

        // Returns a page id from the free list, or extends the file.
        page_id_t AllocatePage();

        // Push a page onto the free list. Caller must ensure the page is not
        // cached in a buffer pool.
        bool DeallocatePage(page_id_t page_id);

        bool Sync();

        uint32_t GetPageCount() const { return page_count_; }
        page_id_t GetFreeListHead() const { return free_list_head_; }
        page_id_t GetCatalogRoot() const { return catalog_root_; }
        bool SetCatalogRoot(page_id_t page_id);

    private:
        bool WriteHeader();
        bool ReadHeader();

        // Raw page access for either backend (page 0 included)
        bool RawRead(page_id_t page_id, char *out);
        bool RawWrite(page_id_t page_id, const char *data);

        int fd_ = -1;
        bool in_memory_ = false;
        std::vector<std::unique_ptr<std::array<char, PAGE_SIZE>>> mem_pages_;
        uint32_t page_count_ = 0;
        page_id_t free_list_head_ = INVALID_PAGE_ID;
        page_id_t catalog_root_ = INVALID_PAGE_ID;
    };

} // namespace sql
