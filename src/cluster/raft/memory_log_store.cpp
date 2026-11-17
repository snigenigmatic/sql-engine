#include "cluster/raft/memory_log_store.h"

namespace sql::cluster
{

    using nuraft::buffer;
    using nuraft::log_entry;
    using nuraft::ptr;
    using nuraft::ulong;

    namespace
    {
        // Callers may modify what they get back, and the store keeps what it
        // is given, so entries are copied both ways
        ptr<log_entry> Clone(const ptr<log_entry> &entry)
        {
            return nuraft::cs_new<log_entry>(entry->get_term(), buffer::clone(entry->get_buf()),
                                             entry->get_val_type(), entry->get_timestamp());
        }
    } // namespace

    MemoryLogStore::MemoryLogStore() : dummy_(nuraft::cs_new<log_entry>(0, buffer::alloc(sizeof(ulong)))) {}

    ptr<log_entry> MemoryLogStore::At(ulong index) const
    {
        if (index < start_ || index >= start_ + entries_.size())
            return dummy_;
        return entries_[index - start_];
    }

    ulong MemoryLogStore::next_slot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_ + entries_.size();
    }

    ulong MemoryLogStore::start_index() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_;
    }

    ptr<log_entry> MemoryLogStore::last_entry() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return Clone(entries_.empty() ? dummy_ : entries_.back());
    }

    ulong MemoryLogStore::append(ptr<log_entry> &entry)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(Clone(entry));
        return start_ + entries_.size() - 1;
    }

    void MemoryLogStore::write_at(ulong index, ptr<log_entry> &entry)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Everything from index on is replaced by this one entry
        if (index < start_)
        {
            entries_.clear();
            start_ = index;
        }
        else if (index - start_ < entries_.size())
            entries_.resize(index - start_);
        entries_.push_back(Clone(entry));
    }

    ptr<std::vector<ptr<log_entry>>> MemoryLogStore::log_entries(ulong start, ulong end)
    {
        auto result = nuraft::cs_new<std::vector<ptr<log_entry>>>();
        std::lock_guard<std::mutex> lock(mutex_);
        for (ulong i = start; i < end; ++i)
            result->push_back(Clone(At(i)));
        return result;
    }

    ptr<log_entry> MemoryLogStore::entry_at(ulong index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return Clone(At(index));
    }

    ulong MemoryLogStore::term_at(ulong index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return At(index)->get_term();
    }

    // count, then each entry as (size, serialized entry)
    ptr<buffer> MemoryLogStore::pack(ulong index, nuraft::int32 count)
    {
        std::vector<ptr<buffer>> serialized;
        size_t total = sizeof(nuraft::int32);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (ulong i = index; i < index + static_cast<ulong>(count); ++i)
            {
                serialized.push_back(At(i)->serialize());
                total += sizeof(nuraft::int32) + serialized.back()->size();
            }
        }
        ptr<buffer> out = buffer::alloc(total);
        out->put(static_cast<nuraft::int32>(serialized.size()));
        for (const auto &entry : serialized)
        {
            out->put(static_cast<nuraft::int32>(entry->size()));
            out->put(*entry);
        }
        out->pos(0);
        return out;
    }

    void MemoryLogStore::apply_pack(ulong index, buffer &pack)
    {
        pack.pos(0);
        const nuraft::int32 count = pack.get_int();
        std::lock_guard<std::mutex> lock(mutex_);
        // The pack replaces everything from index on
        if (index < start_ || index > start_ + entries_.size())
        {
            entries_.clear();
            start_ = index;
        }
        else
            entries_.resize(index - start_);
        for (nuraft::int32 i = 0; i < count; ++i)
        {
            const nuraft::int32 size = pack.get_int();
            ptr<buffer> entry = buffer::alloc(size);
            pack.get(entry);
            entries_.push_back(log_entry::deserialize(*entry));
        }
    }

    bool MemoryLogStore::compact(ulong last_log_index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_log_index < start_)
            return true;
        const ulong drop = last_log_index - start_ + 1;
        if (drop >= entries_.size())
            entries_.clear();
        else
            entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(drop));
        // Even when nothing was stored up to there, the log now starts after it
        start_ = last_log_index + 1;
        return true;
    }

} // namespace sql::cluster
