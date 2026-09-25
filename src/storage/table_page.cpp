#include "storage/table_page.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace sql
{

    namespace
    {
        constexpr size_t OFF_LSN = 0;
        constexpr size_t OFF_NEXT = 4;
        constexpr size_t OFF_SLOT_COUNT = 8;
        constexpr size_t OFF_FREE_PTR = 10;
    } // namespace

    template <typename T>
    T TablePage::ReadAt(size_t offset) const
    {
        T value;
        std::memcpy(&value, data_ + offset, sizeof(T));
        return value;
    }

    template <typename T>
    void TablePage::WriteAt(size_t offset, T value)
    {
        std::memcpy(data_ + offset, &value, sizeof(T));
    }

    void TablePage::Init()
    {
        std::memset(data_, 0, PAGE_SIZE);
        WriteAt<uint32_t>(OFF_LSN, 0);
        SetNextPageId(INVALID_PAGE_ID);
        SetSlotCount(0);
        SetFreePointer(static_cast<uint16_t>(PAGE_SIZE));
    }

    page_id_t TablePage::GetNextPageId() const { return ReadAt<page_id_t>(OFF_NEXT); }
    void TablePage::SetNextPageId(page_id_t page_id) { WriteAt<page_id_t>(OFF_NEXT, page_id); }
    uint16_t TablePage::GetSlotCount() const { return ReadAt<uint16_t>(OFF_SLOT_COUNT); }
    void TablePage::SetSlotCount(uint16_t count) { WriteAt<uint16_t>(OFF_SLOT_COUNT, count); }

    uint16_t TablePage::GetFreePointer() const { return ReadAt<uint16_t>(OFF_FREE_PTR); }
    void TablePage::SetFreePointer(uint16_t ptr) { WriteAt<uint16_t>(OFF_FREE_PTR, ptr); }

    void TablePage::GetSlot(uint16_t slot, uint16_t *offset, uint16_t *size) const
    {
        const size_t base = HEADER_SIZE + slot * SLOT_SIZE;
        *offset = ReadAt<uint16_t>(base);
        *size = ReadAt<uint16_t>(base + 2);
    }

    void TablePage::SetSlot(uint16_t slot, uint16_t offset, uint16_t size)
    {
        const size_t base = HEADER_SIZE + slot * SLOT_SIZE;
        WriteAt<uint16_t>(base, offset);
        WriteAt<uint16_t>(base + 2, size);
    }

    size_t TablePage::GetFreeSpace() const
    {
        const size_t slots_end = HEADER_SIZE + GetSlotCount() * SLOT_SIZE;
        return GetFreePointer() - slots_end;
    }

    size_t TablePage::GetFragmentedSpace() const
    {
        size_t live = 0;
        for (uint16_t i = 0; i < GetSlotCount(); ++i)
        {
            uint16_t offset, size;
            GetSlot(i, &offset, &size);
            if (offset != 0)
                live += size;
        }
        return (PAGE_SIZE - GetFreePointer()) - live;
    }

    void TablePage::Compact()
    {
        struct Live
        {
            uint16_t slot;
            uint16_t offset;
            uint16_t size;
        };
        std::vector<Live> live;
        for (uint16_t i = 0; i < GetSlotCount(); ++i)
        {
            uint16_t offset, size;
            GetSlot(i, &offset, &size);
            if (offset != 0)
                live.push_back({i, offset, size});
        }

        // Move tuples toward the end of the page, highest offset first, so a
        // move never overwrites a tuple that has not been moved yet.
        std::sort(live.begin(), live.end(), [](const Live &a, const Live &b)
                  { return a.offset > b.offset; });

        size_t ptr = PAGE_SIZE;
        for (const auto &t : live)
        {
            ptr -= t.size;
            std::memmove(data_ + ptr, data_ + t.offset, t.size);
            SetSlot(t.slot, static_cast<uint16_t>(ptr), t.size);
        }
        SetFreePointer(static_cast<uint16_t>(ptr));
    }

    bool TablePage::InsertTuple(const char *tuple, uint16_t size, uint16_t *slot)
    {
        if (size == 0 || size > MAX_TUPLE_SIZE)
            return false;

        const size_t needed = static_cast<size_t>(size) + SLOT_SIZE;
        if (GetFreeSpace() < needed)
        {
            if (GetFreeSpace() + GetFragmentedSpace() < needed)
                return false;
            Compact();
        }

        const uint16_t new_slot = GetSlotCount();
        const uint16_t offset = static_cast<uint16_t>(GetFreePointer() - size);
        std::memcpy(data_ + offset, tuple, size);
        SetFreePointer(offset);
        SetSlotCount(static_cast<uint16_t>(new_slot + 1));
        SetSlot(new_slot, offset, size);
        *slot = new_slot;
        return true;
    }

    bool TablePage::IsLive(uint16_t slot) const
    {
        if (slot >= GetSlotCount())
            return false;
        uint16_t offset, size;
        GetSlot(slot, &offset, &size);
        return offset != 0;
    }

    bool TablePage::GetTuple(uint16_t slot, const char **tuple, uint16_t *size) const
    {
        if (!IsLive(slot))
            return false;
        uint16_t offset;
        GetSlot(slot, &offset, size);
        *tuple = data_ + offset;
        return true;
    }

    bool TablePage::UpdateTuple(uint16_t slot, const char *tuple, uint16_t size)
    {
        if (!IsLive(slot) || size == 0 || size > MAX_TUPLE_SIZE)
            return false;

        uint16_t old_offset, old_size;
        GetSlot(slot, &old_offset, &old_size);

        if (size <= old_size)
        {
            // Shrinking or same size: overwrite in place. The leftover bytes
            // become fragmented space reclaimed by the next compaction.
            std::memcpy(data_ + old_offset, tuple, size);
            SetSlot(slot, old_offset, size);
            return true;
        }

        // Growing: the old copy's bytes become reclaimable once it is dropped
        if (GetFreeSpace() < size)
        {
            if (GetFreeSpace() + GetFragmentedSpace() + old_size < size)
                return false;
            SetSlot(slot, 0, 0);
            Compact();
        }
        else
        {
            SetSlot(slot, 0, 0);
        }

        const uint16_t offset = static_cast<uint16_t>(GetFreePointer() - size);
        std::memcpy(data_ + offset, tuple, size);
        SetFreePointer(offset);
        SetSlot(slot, offset, size);
        return true;
    }

    bool TablePage::DeleteTuple(uint16_t slot)
    {
        if (!IsLive(slot))
            return false;
        SetSlot(slot, 0, 0);

        // Trim trailing empty slots so their slot-array space is reusable
        uint16_t count = GetSlotCount();
        while (count > 0 && !IsLive(static_cast<uint16_t>(count - 1)))
            --count;
        SetSlotCount(count);
        return true;
    }

} // namespace sql
