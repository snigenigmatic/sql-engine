#pragma once

#include "common/tuple.h"
#include "storage/buffer_pool.h"
#include "storage/rid.h"
#include <memory>

namespace sql
{

    // A table's rows stored as a singly linked chain of slotted pages.
    // New rows are appended to the last page; a new page is linked in when it
    // is full.
    class TableHeap
    {
    public:
        // Allocate a new, empty heap. Throws on allocation failure.
        static std::unique_ptr<TableHeap> Create(BufferPoolManager *bpm);

        // Attach to an existing heap starting at first_page_id.
        TableHeap(BufferPoolManager *bpm, page_id_t first_page_id);

        page_id_t GetFirstPageId() const { return first_page_id_; }

        // Throws std::runtime_error if the row is too large for a page or the
        // buffer pool is exhausted.
        RID InsertTuple(const Tuple &tuple);

        bool GetTuple(const RID &rid, Tuple *tuple) const;

        // Update in place when the new version fits on the same page;
        // otherwise delete and re-insert. *new_rid receives the final location.
        bool UpdateTuple(const RID &rid, const Tuple &tuple, RID *new_rid);

        bool DeleteTuple(const RID &rid);

        // Iteration: locate the first live tuple, or the next one after `rid`.
        bool FirstTuple(RID *rid, Tuple *tuple) const;
        bool NextTuple(const RID &rid, RID *next_rid, Tuple *tuple) const;

        // Return every page to the free list. The heap is unusable afterwards.
        void Drop();

    private:
        PageGuard FetchOrThrow(page_id_t page_id) const;

        // Scan the chain starting at page_id / slot for the first live tuple
        bool ScanFrom(page_id_t page_id, uint32_t slot, RID *rid, Tuple *tuple) const;

        BufferPoolManager *bpm_;
        page_id_t first_page_id_;
        page_id_t last_page_id_;
    };

} // namespace sql
