#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>

namespace sql
{

    // Page size constants
    constexpr size_t PAGE_SIZE = 4096;

    using page_id_t = int32_t;
    using frame_id_t = int32_t;
    constexpr page_id_t INVALID_PAGE_ID = -1;

    // A single in-memory frame holding one on-disk page. Bookkeeping fields
    // (page id, pin count, dirty flag) are owned by the BufferPoolManager.
    class Page
    {
        friend class BufferPoolManager;

    public:
        Page() { ResetMemory(); }

        char *GetData() { return data_.data(); }
        const char *GetData() const { return data_.data(); }

        page_id_t GetPageId() const { return page_id_; }
        int GetPinCount() const { return pin_count_; }
        bool IsDirty() const { return is_dirty_; }

        // Typed access to a fixed offset inside the page
        template <typename T>
        T Read(size_t offset) const
        {
            T value;
            std::memcpy(&value, data_.data() + offset, sizeof(T));
            return value;
        }

        template <typename T>
        void Write(size_t offset, const T &value)
        {
            std::memcpy(data_.data() + offset, &value, sizeof(T));
        }

    private:
        void ResetMemory() { data_.fill(0); }

        std::array<char, PAGE_SIZE> data_;
        page_id_t page_id_ = INVALID_PAGE_ID;
        int pin_count_ = 0;
        bool is_dirty_ = false;
    };

} // namespace sql
