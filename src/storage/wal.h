#pragma once

#include "storage/page.h"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace sql
{

    // Write-ahead log of whole page images (the design SQLite's WAL mode
    // uses). A transaction appends the new version of every page it changed,
    // ending with a commit frame; the database file itself is only updated
    // by a checkpoint, which copies committed pages back into it. Readers see
    // the newest version of a page: this transaction's frames, then committed
    // frames, then the database file.
    //
    // File layout:
    //   header (32 bytes): magic "SQLWAL01", version, page size, salt1,
    //                      salt2, checksum of the preceding 24 bytes
    //   frames: { page_id, commit_page_count, salt1, salt2, checksum } +
    //           page image
    // commit_page_count is non-zero only on a commit frame and records the
    // database size after the commit. checksum is cumulative (it covers every
    // earlier frame too), so recovery stops at the first torn or stale frame
    // and anything after the last intact commit frame is discarded. The salts
    // change every time the log is reset, so frames left over from an older
    // log generation never validate.
    class WriteAheadLog
    {
    public:
        static constexpr size_t HEADER_SIZE = 32;
        static constexpr size_t FRAME_HEADER_SIZE = 24;
        static constexpr size_t FRAME_SIZE = FRAME_HEADER_SIZE + PAGE_SIZE;

        WriteAheadLog() = default;
        ~WriteAheadLog() { Close(false); }

        WriteAheadLog(const WriteAheadLog &) = delete;
        WriteAheadLog &operator=(const WriteAheadLog &) = delete;

        // Open (or create) the log and recover every committed frame. An
        // incomplete or corrupt tail is truncated away.
        bool Open(const std::string &path);
        void Close(bool remove_file);
        bool IsOpen() const { return fd_ >= 0; }

        // Copy the newest version of a page held in the log into out.
        // Returns false if the log has no version of the page.
        bool ReadPage(page_id_t page_id, char *out) const;

        // Append a page image. A non-zero commit_page_count makes it a commit
        // frame: the log is fsynced and all frames since the previous commit
        // become committed.
        bool AppendFrame(page_id_t page_id, const char *data, uint32_t commit_page_count);

        // Discard frames appended since the last commit
        bool Rollback();

        // A point inside the open transaction that it can be rolled back to
        struct Savepoint
        {
            uint64_t frame_count = 0;
            uint64_t checksum = 0;
            std::unordered_map<page_id_t, uint64_t> pending;
        };
        Savepoint CreateSavepoint() const { return {frame_count_, running_checksum_, pending_}; }

        // Discard frames appended after the savepoint. Fails if a commit
        // happened since it was taken.
        bool RollbackTo(const Savepoint &savepoint);

        // Empty the log (after a checkpoint) and start a new generation
        bool Reset();

        bool Sync();

        // Pages whose newest committed version is in the log, and the frame
        // holding it
        const std::unordered_map<page_id_t, uint64_t> &GetCommittedPages() const { return committed_; }
        bool ReadFrameData(uint64_t frame_no, char *out) const;

        uint64_t GetFrameCount() const { return frame_count_; }
        uint64_t GetCommittedFrameCount() const { return committed_frames_; }
        bool HasUncommittedFrames() const { return frame_count_ != committed_frames_; }

        // Database page count recorded by the last commit (0 if none)
        uint32_t GetCommittedPageCount() const { return committed_page_count_; }

    private:
        enum class RecoverResult
        {
            NO_LOG,    // missing or invalid header: start a new log
            RECOVERED, // committed frames (possibly none) are available
            IO_ERROR,
        };
        RecoverResult Recover();
        static uint64_t FrameOffset(uint64_t frame_no) { return HEADER_SIZE + frame_no * FRAME_SIZE; }

        std::string path_;
        int fd_ = -1;
        uint32_t salt1_ = 0;
        uint32_t salt2_ = 0;

        uint64_t frame_count_ = 0;
        uint64_t running_checksum_ = 0;

        uint64_t committed_frames_ = 0;
        uint64_t committed_checksum_ = 0;
        uint32_t committed_page_count_ = 0;

        // page -> newest frame, for committed frames and for the open
        // (uncommitted) transaction
        std::unordered_map<page_id_t, uint64_t> committed_;
        std::unordered_map<page_id_t, uint64_t> pending_;
    };

} // namespace sql
