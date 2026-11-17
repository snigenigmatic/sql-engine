#pragma once

#include "cluster/raft/nuraft.h"
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace sql::cluster
{

    // A Raft log kept in one file, <dir>/log, surviving restarts:
    //
    //   header   "SQLRLOG1" | u64 start index
    //   records  one per entry from the start index on:
    //            u32 size | u32 crc32 | log_entry::serialize()
    //
    // Appends go to the end and are fsynced at the end of every append batch
    // (NuRaft acknowledges entries only after that) and on overwrites. A
    // change of start index (compaction, or a log restarting at an index it
    // does not hold) rewrites the whole file under a temporary name and
    // renames it into place, so the start and the entries can never
    // disagree after a crash. On open, a torn or corrupt tail (a crash
    // mid-append) is cut off. Entries are also kept in memory: logs are
    // bounded by snapshots and compaction.
    class FileLogStore : public nuraft::log_store
    {
    public:
        // Opens or creates the log in `directory` (which must exist).
        // Throws std::runtime_error if the files cannot be read or written.
        explicit FileLogStore(const std::string &directory);
        ~FileLogStore() override;

        nuraft::ulong next_slot() const override;
        nuraft::ulong start_index() const override;
        nuraft::ptr<nuraft::log_entry> last_entry() const override;
        nuraft::ulong append(nuraft::ptr<nuraft::log_entry> &entry) override;
        void write_at(nuraft::ulong index, nuraft::ptr<nuraft::log_entry> &entry) override;
        void end_of_append_batch(nuraft::ulong start, nuraft::ulong count) override;
        nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>> log_entries(nuraft::ulong start,
                                                                             nuraft::ulong end) override;
        nuraft::ptr<nuraft::log_entry> entry_at(nuraft::ulong index) override;
        nuraft::ulong term_at(nuraft::ulong index) override;
        nuraft::ptr<nuraft::buffer> pack(nuraft::ulong index, nuraft::int32 count) override;
        void apply_pack(nuraft::ulong index, nuraft::buffer &pack) override;
        bool compact(nuraft::ulong last_log_index) override;
        bool flush() override;

        // Records dropped as torn or corrupt when the log was opened
        size_t DroppedOnOpen() const { return dropped_on_open_; }

    private:
        // Caller holds mutex_ for all of these
        nuraft::ptr<nuraft::log_entry> At(nuraft::ulong index) const;
        void AppendRecord(const nuraft::ptr<nuraft::log_entry> &entry);
        void TruncateFrom(nuraft::ulong index); // drops entries index.. from file and memory
        void Sync();
        // Replaces the file with the header and the entries held now
        void RewriteFile();

        std::string dir_;
        int fd_ = -1;
        mutable std::mutex mutex_;
        nuraft::ulong start_ = 1;
        std::deque<nuraft::ptr<nuraft::log_entry>> entries_;
        std::vector<uint64_t> offsets_; // file offset of each entry's record
        uint64_t end_ = 0;              // file size
        nuraft::ptr<nuraft::log_entry> dummy_;
        size_t dropped_on_open_ = 0;
    };

    uint32_t Crc32(const void *data, size_t size);

    // Writes a whole file and makes it durable under its final name: write
    // to <path>.tmp, fsync, rename over <path>, fsync the directory
    void WriteFileAtomically(const std::string &path, const std::string &contents);

    // The whole file, or false if it does not exist
    bool ReadFile(const std::string &path, std::string *contents);

} // namespace sql::cluster
