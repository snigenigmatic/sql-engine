#pragma once

#include "storage/page.h"
#include <cstdint>
#include <functional>
#include <string>

namespace sql
{

    // Record identifier: physical location of a tuple (page + slot).
    // Temporary in-memory tables use page_id == INVALID_PAGE_ID and the slot
    // as a plain row position.
    struct RID
    {
        page_id_t page_id = INVALID_PAGE_ID;
        uint32_t slot = 0;

        RID() = default;
        RID(page_id_t p, uint32_t s) : page_id(p), slot(s) {}

        bool operator==(const RID &other) const { return page_id == other.page_id && slot == other.slot; }
        bool operator!=(const RID &other) const { return !(*this == other); }
        bool operator<(const RID &other) const
        {
            return page_id != other.page_id ? page_id < other.page_id : slot < other.slot;
        }

        std::string ToString() const
        {
            return "(" + std::to_string(page_id) + "," + std::to_string(slot) + ")";
        }
    };

    struct RIDHasher
    {
        size_t operator()(const RID &rid) const
        {
            // Shift as unsigned: page_id may be INVALID_PAGE_ID (-1)
            const uint64_t page = static_cast<uint32_t>(rid.page_id);
            return std::hash<uint64_t>{}((page << 32) | rid.slot);
        }
    };

} // namespace sql
