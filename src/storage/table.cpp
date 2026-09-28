#include "storage/table.h"
#include <stdexcept>

namespace sql
{

    // ── TableIterator ────────────────────────────────────────────────────────

    TableIterator &TableIterator::operator++()
    {
        if (!at_end_ && table_)
            at_end_ = !table_->Advance(&rid_, &rid_, &tuple_);
        return *this;
    }

    // ── Table ────────────────────────────────────────────────────────────────

    Table::Table(std::string name, Schema schema, std::unique_ptr<TableHeap> heap)
        : name_(std::move(name)), schema_(std::move(schema)), heap_(std::move(heap))
    {
        RID rid;
        Tuple tuple;
        for (bool ok = heap_->FirstTuple(&rid, &tuple); ok; ok = heap_->NextTuple(rid, &rid, &tuple))
            ++tuple_count_;
    }

    RID Table::Insert(const Tuple &tuple)
    {
        RID rid;
        if (heap_)
        {
            rid = heap_->InsertTuple(tuple);
        }
        else
        {
            rid = RID(INVALID_PAGE_ID, static_cast<uint32_t>(temp_rows_.size()));
            temp_rows_.emplace_back(tuple);
        }
        ++tuple_count_;
        return rid;
    }

    bool Table::GetTuple(const RID &rid, Tuple *tuple) const
    {
        if (heap_)
            return heap_->GetTuple(rid, tuple);
        if (rid.page_id != INVALID_PAGE_ID || rid.slot >= temp_rows_.size() || !temp_rows_[rid.slot])
            return false;
        *tuple = *temp_rows_[rid.slot];
        return true;
    }

    bool Table::UpdateTuple(const RID &rid, const Tuple &tuple, RID *new_rid)
    {
        if (heap_)
            return heap_->UpdateTuple(rid, tuple, new_rid);
        if (rid.page_id != INVALID_PAGE_ID || rid.slot >= temp_rows_.size() || !temp_rows_[rid.slot])
            return false;
        temp_rows_[rid.slot] = tuple;
        if (new_rid)
            *new_rid = rid;
        return true;
    }

    bool Table::DeleteTuple(const RID &rid)
    {
        bool deleted = false;
        if (heap_)
        {
            deleted = heap_->DeleteTuple(rid);
        }
        else if (rid.page_id == INVALID_PAGE_ID && rid.slot < temp_rows_.size() && temp_rows_[rid.slot])
        {
            temp_rows_[rid.slot].reset();
            deleted = true;
        }
        if (deleted)
            --tuple_count_;
        return deleted;
    }

    void Table::Drop()
    {
        if (heap_)
        {
            heap_->Drop();
            heap_.reset();
        }
        temp_rows_.clear();
        tuple_count_ = 0;
    }

    bool Table::Advance(const RID *after, RID *rid, Tuple *tuple) const
    {
        if (heap_)
            return after ? heap_->NextTuple(*after, rid, tuple) : heap_->FirstTuple(rid, tuple);

        size_t pos = after ? static_cast<size_t>(after->slot) + 1 : 0;
        for (; pos < temp_rows_.size(); ++pos)
        {
            if (temp_rows_[pos])
            {
                *rid = RID(INVALID_PAGE_ID, static_cast<uint32_t>(pos));
                *tuple = *temp_rows_[pos];
                return true;
            }
        }
        return false;
    }

    TableIterator Table::begin() const
    {
        TableIterator it(this, false);
        it.at_end_ = !Advance(nullptr, &it.rid_, &it.tuple_);
        return it;
    }

    int Table::GetColumnIndex(const std::string &column_name) const
    {
        const auto &columns = schema_.GetColumns();
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (columns[i].name == column_name)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

} // namespace sql
