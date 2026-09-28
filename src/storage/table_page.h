#pragma once

#include "storage/page.h"
#include <cstdint>

namespace sql
{

    // View over a Page laid out as a slotted heap page.
    //
    //   [0..4)   page LSN (reserved for the WAL)
    //   [4..8)   next page id in the table's page chain
    //   [8..10)  slot count
    //   [10..12) free space pointer: start of the tuple data region
    //   [12..16) reserved
    //   [16..)   slot array, 4 bytes per slot: u16 offset, u16 length
    //   ...      free space ...
    //   [free space pointer..PAGE_SIZE) tuple data, growing downward
    //
    // A slot with offset 0 is empty (deleted). Slot ids are stable for the
    // lifetime of a tuple; they are never reassigned to a different tuple
    // except when trailing empty slots are trimmed.
    class TablePage
    {
    public:
        static constexpr size_t HEADER_SIZE = 16;
        static constexpr size_t SLOT_SIZE = 4;
        static constexpr size_t MAX_TUPLE_SIZE = PAGE_SIZE - HEADER_SIZE - SLOT_SIZE;

        explicit TablePage(char *data) : data_(data) {}

        // Format a freshly allocated page
        void Init();

        page_id_t GetNextPageId() const;
        void SetNextPageId(page_id_t page_id);

        uint16_t GetSlotCount() const;

        // Contiguous free bytes between the slot array and tuple data
        size_t GetFreeSpace() const;

        // Insert into a new slot at the end of the slot array. Compacts the
        // page if fragmented space would make it fit. Returns false if full.
        bool InsertTuple(const char *tuple, uint16_t size, uint16_t *slot);

        // Returns false for an out-of-range or empty slot.
        bool GetTuple(uint16_t slot, const char **tuple, uint16_t *size) const;

        // Replace a tuple, keeping its slot. Returns false if the new version
        // does not fit on this page (caller should move it elsewhere).
        bool UpdateTuple(uint16_t slot, const char *tuple, uint16_t size);

        bool DeleteTuple(uint16_t slot);

        bool IsLive(uint16_t slot) const;

    private:
        uint16_t GetFreePointer() const;
        void SetFreePointer(uint16_t ptr);
        void SetSlotCount(uint16_t count);
        void GetSlot(uint16_t slot, uint16_t *offset, uint16_t *size) const;
        void SetSlot(uint16_t slot, uint16_t offset, uint16_t size);

        // Bytes reclaimable by compaction (dead tuple space)
        size_t GetFragmentedSpace() const;

        // Rewrite live tuples contiguously at the end of the page
        void Compact();

        template <typename T>
        T ReadAt(size_t offset) const;
        template <typename T>
        void WriteAt(size_t offset, T value);

        char *data_;
    };

} // namespace sql
