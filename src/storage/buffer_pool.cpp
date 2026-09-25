#include "storage/buffer_pool.h"

namespace sql
{

    // ── PageGuard ────────────────────────────────────────────────────────────

    PageGuard::PageGuard(PageGuard &&other) noexcept
        : bpm_(other.bpm_), page_(other.page_), dirty_(other.dirty_)
    {
        other.bpm_ = nullptr;
        other.page_ = nullptr;
        other.dirty_ = false;
    }

    PageGuard &PageGuard::operator=(PageGuard &&other) noexcept
    {
        if (this != &other)
        {
            Release();
            bpm_ = other.bpm_;
            page_ = other.page_;
            dirty_ = other.dirty_;
            other.bpm_ = nullptr;
            other.page_ = nullptr;
            other.dirty_ = false;
        }
        return *this;
    }

    void PageGuard::Release()
    {
        if (bpm_ && page_)
            bpm_->UnpinPage(page_->GetPageId(), dirty_);
        bpm_ = nullptr;
        page_ = nullptr;
        dirty_ = false;
    }

    // ── BufferPoolManager ────────────────────────────────────────────────────

    BufferPoolManager::BufferPoolManager(size_t pool_size, Pager *pager)
        : pager_(pager), frames_(pool_size)
    {
        for (size_t i = 0; i < pool_size; ++i)
            free_frames_.push_back(static_cast<frame_id_t>(i));
    }

    BufferPoolManager::~BufferPoolManager()
    {
        FlushAll();
    }

    void BufferPoolManager::ReplacerRemove(frame_id_t frame_id)
    {
        auto it = lru_pos_.find(frame_id);
        if (it != lru_pos_.end())
        {
            lru_.erase(it->second);
            lru_pos_.erase(it);
        }
    }

    void BufferPoolManager::ReplacerAdd(frame_id_t frame_id)
    {
        ReplacerRemove(frame_id);
        lru_.push_back(frame_id);
        lru_pos_[frame_id] = std::prev(lru_.end());
    }

    bool BufferPoolManager::FlushFrame(frame_id_t frame_id)
    {
        Page &page = frames_[frame_id];
        if (page.page_id_ == INVALID_PAGE_ID || !page.is_dirty_)
            return true;
        if (!pager_->WritePage(page.page_id_, page.GetData()))
            return false;
        page.is_dirty_ = false;
        return true;
    }

    bool BufferPoolManager::AcquireFrame(frame_id_t *frame_id)
    {
        if (!free_frames_.empty())
        {
            *frame_id = free_frames_.front();
            free_frames_.pop_front();
            return true;
        }

        if (lru_.empty())
            return false;

        frame_id_t victim = lru_.front();
        if (!FlushFrame(victim))
            return false;
        ReplacerRemove(victim);

        Page &page = frames_[victim];
        page_table_.erase(page.page_id_);
        page.page_id_ = INVALID_PAGE_ID;
        page.pin_count_ = 0;
        page.is_dirty_ = false;
        *frame_id = victim;
        return true;
    }

    Page *BufferPoolManager::FetchPage(page_id_t page_id)
    {
        std::lock_guard<std::recursive_mutex> lock(latch_);

        auto it = page_table_.find(page_id);
        if (it != page_table_.end())
        {
            Page &page = frames_[it->second];
            page.pin_count_++;
            ReplacerRemove(it->second);
            return &page;
        }

        frame_id_t frame_id;
        if (!AcquireFrame(&frame_id))
            return nullptr;

        Page &page = frames_[frame_id];
        if (!pager_->ReadPage(page_id, page.GetData()))
        {
            page.ResetMemory();
            free_frames_.push_back(frame_id);
            return nullptr;
        }

        page.page_id_ = page_id;
        page.pin_count_ = 1;
        page.is_dirty_ = false;
        page_table_[page_id] = frame_id;
        return &page;
    }

    Page *BufferPoolManager::NewPage(page_id_t *page_id)
    {
        std::lock_guard<std::recursive_mutex> lock(latch_);

        frame_id_t frame_id;
        if (!AcquireFrame(&frame_id))
            return nullptr;

        page_id_t new_id = pager_->AllocatePage();
        if (new_id == INVALID_PAGE_ID)
        {
            free_frames_.push_back(frame_id);
            return nullptr;
        }

        Page &page = frames_[frame_id];
        page.ResetMemory();
        page.page_id_ = new_id;
        page.pin_count_ = 1;
        page.is_dirty_ = true; // must reach disk even if caller writes nothing
        page_table_[new_id] = frame_id;
        *page_id = new_id;
        return &page;
    }

    bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty)
    {
        std::lock_guard<std::recursive_mutex> lock(latch_);

        auto it = page_table_.find(page_id);
        if (it == page_table_.end())
            return false;

        Page &page = frames_[it->second];
        if (page.pin_count_ <= 0)
            return false;

        if (is_dirty)
            page.is_dirty_ = true;
        if (--page.pin_count_ == 0)
            ReplacerAdd(it->second);
        return true;
    }

    bool BufferPoolManager::FlushPage(page_id_t page_id)
    {
        std::lock_guard<std::recursive_mutex> lock(latch_);

        auto it = page_table_.find(page_id);
        if (it == page_table_.end())
            return false;
        return FlushFrame(it->second);
    }

    bool BufferPoolManager::FlushAll()
    {
        std::lock_guard<std::recursive_mutex> lock(latch_);

        bool ok = true;
        for (const auto &entry : page_table_)
            ok = FlushFrame(entry.second) && ok;
        return pager_->Sync() && ok;
    }

    bool BufferPoolManager::DeletePage(page_id_t page_id)
    {
        std::lock_guard<std::recursive_mutex> lock(latch_);

        auto it = page_table_.find(page_id);
        if (it != page_table_.end())
        {
            frame_id_t frame_id = it->second;
            Page &page = frames_[frame_id];
            if (page.pin_count_ > 0)
                return false;
            ReplacerRemove(frame_id);
            page_table_.erase(it);
            page.ResetMemory();
            page.page_id_ = INVALID_PAGE_ID;
            page.is_dirty_ = false;
            free_frames_.push_back(frame_id);
        }
        return pager_->DeallocatePage(page_id);
    }

    PageGuard BufferPoolManager::FetchPageGuarded(page_id_t page_id)
    {
        return PageGuard(this, FetchPage(page_id));
    }

    PageGuard BufferPoolManager::NewPageGuarded(page_id_t *page_id)
    {
        return PageGuard(this, NewPage(page_id));
    }

} // namespace sql
