#pragma once

#include "cluster/raft/nuraft.h"
#include <deque>
#include <mutex>

namespace sql::cluster
{

    // A Raft log kept in memory. Entries live at indexes
    // [start_index(), next_slot()); index 0 is never stored, and a dummy
    // entry with term 0 stands in for anything outside that range. Used by
    // tests and as the reference for the file-backed store.
    class MemoryLogStore : public nuraft::log_store
    {
    public:
        MemoryLogStore();

        nuraft::ulong next_slot() const override;
        nuraft::ulong start_index() const override;
        nuraft::ptr<nuraft::log_entry> last_entry() const override;
        nuraft::ulong append(nuraft::ptr<nuraft::log_entry> &entry) override;
        void write_at(nuraft::ulong index, nuraft::ptr<nuraft::log_entry> &entry) override;
        nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>> log_entries(nuraft::ulong start,
                                                                             nuraft::ulong end) override;
        nuraft::ptr<nuraft::log_entry> entry_at(nuraft::ulong index) override;
        nuraft::ulong term_at(nuraft::ulong index) override;
        nuraft::ptr<nuraft::buffer> pack(nuraft::ulong index, nuraft::int32 count) override;
        void apply_pack(nuraft::ulong index, nuraft::buffer &pack) override;
        bool compact(nuraft::ulong last_log_index) override;
        bool flush() override { return true; }

    private:
        // Caller holds mutex_
        nuraft::ptr<nuraft::log_entry> At(nuraft::ulong index) const;

        mutable std::mutex mutex_;
        nuraft::ulong start_ = 1; // index of entries_.front()
        std::deque<nuraft::ptr<nuraft::log_entry>> entries_;
        nuraft::ptr<nuraft::log_entry> dummy_;
    };

} // namespace sql::cluster
