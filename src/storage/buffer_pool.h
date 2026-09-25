#pragma once

#include "storage/page.h"
#include "storage/pager.h"
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sql
{

    class BufferPoolManager;

    // RAII handle for a pinned page. Unpins (propagating the dirty flag) when
    // it goes out of scope. Move-only.
    class PageGuard
    {
    public:
        PageGuard() = default;
        PageGuard(BufferPoolManager *bpm, Page *page) : bpm_(bpm), page_(page) {}
        ~PageGuard() { Release(); }

        PageGuard(const PageGuard &) = delete;
        PageGuard &operator=(const PageGuard &) = delete;
        PageGuard(PageGuard &&other) noexcept;
        PageGuard &operator=(PageGuard &&other) noexcept;

        explicit operator bool() const { return page_ != nullptr; }
        Page *GetPage() const { return page_; }
        page_id_t GetPageId() const { return page_ ? page_->GetPageId() : INVALID_PAGE_ID; }
        char *GetData() { return page_->GetData(); }
        const char *GetData() const { return page_->GetData(); }

        void MarkDirty() { dirty_ = true; }

        // Unpin now instead of at destruction.
        void Release();

    private:
        BufferPoolManager *bpm_ = nullptr;
        Page *page_ = nullptr;
        bool dirty_ = false;
    };

    // Caches a fixed number of pages in memory, evicting the least recently
    // unpinned page when a frame is needed. Dirty pages are written back to the
    // Pager on eviction or flush.
    class BufferPoolManager
    {
    public:
        BufferPoolManager(size_t pool_size, Pager *pager);
        ~BufferPoolManager();

        BufferPoolManager(const BufferPoolManager &) = delete;
        BufferPoolManager &operator=(const BufferPoolManager &) = delete;

        // Pin an existing page. Returns nullptr if every frame is pinned or the
        // page cannot be read.
        Page *FetchPage(page_id_t page_id);

        // Allocate a fresh zeroed page and pin it. Returns nullptr on failure.
        Page *NewPage(page_id_t *page_id);

        // Drop one pin. is_dirty is sticky until the page is written back.
        bool UnpinPage(page_id_t page_id, bool is_dirty);

        bool FlushPage(page_id_t page_id);

        // True if the page is cached and currently pinned
        bool IsPinned(page_id_t page_id);
        bool FlushAll();

        // Remove a page from the pool and return it to the Pager free list.
        // Fails if the page is still pinned.
        bool DeletePage(page_id_t page_id);

        PageGuard FetchPageGuarded(page_id_t page_id);
        PageGuard NewPageGuarded(page_id_t *page_id);

        size_t GetPoolSize() const { return frames_.size(); }

    private:
        // Pick a frame to reuse (free list first, then LRU victim).
        // Writes back a dirty victim. Caller holds latch_.
        bool AcquireFrame(frame_id_t *frame_id);

        void ReplacerRemove(frame_id_t frame_id);
        void ReplacerAdd(frame_id_t frame_id);
        bool FlushFrame(frame_id_t frame_id);

        Pager *pager_;
        std::vector<Page> frames_;
        std::unordered_map<page_id_t, frame_id_t> page_table_;
        std::list<frame_id_t> free_frames_;

        // Evictable frames, least recently unpinned at the front
        std::list<frame_id_t> lru_;
        std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> lru_pos_;

        std::recursive_mutex latch_;
    };

} // namespace sql
