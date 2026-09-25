#include "storage/table_heap.h"
#include "storage/table_page.h"
#include <stdexcept>
#include <string>
#include <vector>

namespace sql
{

    std::unique_ptr<TableHeap> TableHeap::Create(BufferPoolManager *bpm)
    {
        page_id_t page_id;
        PageGuard guard = bpm->NewPageGuarded(&page_id);
        if (!guard)
            throw std::runtime_error("Unable to allocate table page");
        TablePage(guard.GetData()).Init();
        guard.MarkDirty();
        guard.Release();
        return std::make_unique<TableHeap>(bpm, page_id);
    }

    TableHeap::TableHeap(BufferPoolManager *bpm, page_id_t first_page_id)
        : bpm_(bpm), first_page_id_(first_page_id), last_page_id_(first_page_id)
    {
        // Walk the chain once to find the append target
        page_id_t current = first_page_id_;
        while (current != INVALID_PAGE_ID)
        {
            PageGuard guard = FetchOrThrow(current);
            last_page_id_ = current;
            current = TablePage(guard.GetData()).GetNextPageId();
        }
    }

    PageGuard TableHeap::FetchOrThrow(page_id_t page_id) const
    {
        PageGuard guard = bpm_->FetchPageGuarded(page_id);
        if (!guard)
            throw std::runtime_error("Buffer pool exhausted or unreadable page " + std::to_string(page_id));
        return guard;
    }

    RID TableHeap::InsertTuple(const Tuple &tuple)
    {
        std::string bytes;
        tuple.SerializeTo(&bytes);
        if (bytes.size() > TablePage::MAX_TUPLE_SIZE)
            throw std::runtime_error("Row too large: " + std::to_string(bytes.size()) +
                                     " bytes (max " + std::to_string(TablePage::MAX_TUPLE_SIZE) + ")");
        const auto size = static_cast<uint16_t>(bytes.size());

        PageGuard last = FetchOrThrow(last_page_id_);
        uint16_t slot;
        if (TablePage(last.GetData()).InsertTuple(bytes.data(), size, &slot))
        {
            last.MarkDirty();
            return RID(last_page_id_, slot);
        }

        // Last page is full: link a new one
        page_id_t new_page_id;
        PageGuard fresh = bpm_->NewPageGuarded(&new_page_id);
        if (!fresh)
            throw std::runtime_error("Unable to allocate table page");
        TablePage fresh_page(fresh.GetData());
        fresh_page.Init();
        if (!fresh_page.InsertTuple(bytes.data(), size, &slot))
            throw std::runtime_error("Row does not fit on an empty page");
        fresh.MarkDirty();

        TablePage(last.GetData()).SetNextPageId(new_page_id);
        last.MarkDirty();
        last_page_id_ = new_page_id;
        return RID(new_page_id, slot);
    }

    bool TableHeap::GetTuple(const RID &rid, Tuple *tuple) const
    {
        if (rid.page_id == INVALID_PAGE_ID || rid.slot > UINT16_MAX)
            return false;
        PageGuard guard = bpm_->FetchPageGuarded(rid.page_id);
        if (!guard)
            return false;
        const char *data;
        uint16_t size;
        if (!TablePage(guard.GetData()).GetTuple(static_cast<uint16_t>(rid.slot), &data, &size))
            return false;
        return Tuple::DeserializeFrom(data, size, tuple);
    }

    bool TableHeap::UpdateTuple(const RID &rid, const Tuple &tuple, RID *new_rid)
    {
        if (rid.page_id == INVALID_PAGE_ID || rid.slot > UINT16_MAX)
            return false;

        std::string bytes;
        tuple.SerializeTo(&bytes);
        if (bytes.size() > TablePage::MAX_TUPLE_SIZE)
            throw std::runtime_error("Row too large: " + std::to_string(bytes.size()) +
                                     " bytes (max " + std::to_string(TablePage::MAX_TUPLE_SIZE) + ")");

        {
            PageGuard guard = FetchOrThrow(rid.page_id);
            TablePage page(guard.GetData());
            if (!page.IsLive(static_cast<uint16_t>(rid.slot)))
                return false;
            if (page.UpdateTuple(static_cast<uint16_t>(rid.slot), bytes.data(), static_cast<uint16_t>(bytes.size())))
            {
                guard.MarkDirty();
                if (new_rid)
                    *new_rid = rid;
                return true;
            }
        }

        // Does not fit on its page any more: move it
        RID moved = InsertTuple(tuple);
        DeleteTuple(rid);
        if (new_rid)
            *new_rid = moved;
        return true;
    }

    bool TableHeap::DeleteTuple(const RID &rid)
    {
        if (rid.page_id == INVALID_PAGE_ID || rid.slot > UINT16_MAX)
            return false;
        PageGuard guard = bpm_->FetchPageGuarded(rid.page_id);
        if (!guard)
            return false;
        if (!TablePage(guard.GetData()).DeleteTuple(static_cast<uint16_t>(rid.slot)))
            return false;
        guard.MarkDirty();
        return true;
    }

    bool TableHeap::ScanFrom(page_id_t page_id, uint32_t slot, RID *rid, Tuple *tuple) const
    {
        while (page_id != INVALID_PAGE_ID)
        {
            PageGuard guard = FetchOrThrow(page_id);
            TablePage page(guard.GetData());
            for (uint32_t s = slot; s < page.GetSlotCount(); ++s)
            {
                const char *data;
                uint16_t size;
                if (!page.GetTuple(static_cast<uint16_t>(s), &data, &size))
                    continue;
                if (!Tuple::DeserializeFrom(data, size, tuple))
                    throw std::runtime_error("Corrupt tuple at " + RID(page_id, s).ToString());
                *rid = RID(page_id, s);
                return true;
            }
            page_id = page.GetNextPageId();
            slot = 0;
        }
        return false;
    }

    bool TableHeap::FirstTuple(RID *rid, Tuple *tuple) const
    {
        return ScanFrom(first_page_id_, 0, rid, tuple);
    }

    bool TableHeap::NextTuple(const RID &rid, RID *next_rid, Tuple *tuple) const
    {
        return ScanFrom(rid.page_id, rid.slot + 1, next_rid, tuple);
    }

    void TableHeap::Drop()
    {
        std::vector<page_id_t> pages;
        page_id_t current = first_page_id_;
        while (current != INVALID_PAGE_ID)
        {
            PageGuard guard = FetchOrThrow(current);
            pages.push_back(current);
            current = TablePage(guard.GetData()).GetNextPageId();
        }
        for (page_id_t page_id : pages)
        {
            if (bpm_->IsPinned(page_id))
                throw std::runtime_error("Cannot drop table: page " + std::to_string(page_id) + " is in use");
        }
        first_page_id_ = INVALID_PAGE_ID;
        last_page_id_ = INVALID_PAGE_ID;
        for (page_id_t page_id : pages)
        {
            if (!bpm_->DeletePage(page_id))
                throw std::runtime_error("Failed to free table page " + std::to_string(page_id));
        }
    }

} // namespace sql
