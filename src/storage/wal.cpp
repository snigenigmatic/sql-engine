#include "storage/wal.h"
#include <array>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <sys/stat.h>
#include <unistd.h>

namespace sql
{

    namespace
    {
        constexpr char MAGIC[8] = {'S', 'Q', 'L', 'W', 'A', 'L', '0', '1'};
        constexpr uint32_t VERSION = 1;

        // FNV-1a, chained through the previous value
        uint64_t Checksum(uint64_t seed, const char *data, size_t size)
        {
            uint64_t h = seed;
            for (size_t i = 0; i < size; ++i)
            {
                h ^= static_cast<unsigned char>(data[i]);
                h *= 1099511628211ULL;
            }
            return h;
        }

        constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;

        template <typename T>
        void Put(char *buf, size_t offset, T value)
        {
            std::memcpy(buf + offset, &value, sizeof(T));
        }

        template <typename T>
        T Get(const char *buf, size_t offset)
        {
            T value;
            std::memcpy(&value, buf + offset, sizeof(T));
            return value;
        }

        bool PreadFull(int fd, char *buf, size_t len, uint64_t offset)
        {
            size_t done = 0;
            while (done < len)
            {
                ssize_t n = pread(fd, buf + done, len - done, static_cast<off_t>(offset + done));
                if (n <= 0)
                    return false;
                done += static_cast<size_t>(n);
            }
            return true;
        }

        bool PwriteFull(int fd, const char *buf, size_t len, uint64_t offset)
        {
            size_t done = 0;
            while (done < len)
            {
                ssize_t n = pwrite(fd, buf + done, len - done, static_cast<off_t>(offset + done));
                if (n <= 0)
                    return false;
                done += static_cast<size_t>(n);
            }
            return true;
        }
    } // namespace

    bool WriteAheadLog::Open(const std::string &path)
    {
        Close(false);
        path_ = path;
        fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ < 0)
            return false;
        const RecoverResult result = Recover();
        if (result == RecoverResult::IO_ERROR || (result == RecoverResult::NO_LOG && !Reset()))
        {
            Close(false);
            return false;
        }
        return true;
    }

    void WriteAheadLog::Close(bool remove_file)
    {
        if (fd_ >= 0)
        {
            close(fd_);
            fd_ = -1;
            if (remove_file)
                unlink(path_.c_str());
        }
        committed_.clear();
        pending_.clear();
        frame_count_ = committed_frames_ = 0;
        committed_page_count_ = 0;
    }

    WriteAheadLog::RecoverResult WriteAheadLog::Recover()
    {
        std::array<char, HEADER_SIZE> header{};
        if (!PreadFull(fd_, header.data(), HEADER_SIZE, 0) ||
            std::memcmp(header.data(), MAGIC, sizeof(MAGIC)) != 0 ||
            Get<uint32_t>(header.data(), 8) != VERSION ||
            Get<uint32_t>(header.data(), 12) != PAGE_SIZE ||
            Get<uint64_t>(header.data(), 24) != Checksum(FNV_OFFSET, header.data(), 24))
        {
            return RecoverResult::NO_LOG;
        }
        salt1_ = Get<uint32_t>(header.data(), 16);
        salt2_ = Get<uint32_t>(header.data(), 20);

        committed_.clear();
        pending_.clear();
        running_checksum_ = committed_checksum_ = Get<uint64_t>(header.data(), 24);
        frame_count_ = committed_frames_ = 0;
        committed_page_count_ = 0;

        std::array<char, FRAME_SIZE> frame{};
        while (PreadFull(fd_, frame.data(), FRAME_SIZE, FrameOffset(frame_count_)))
        {
            if (Get<uint32_t>(frame.data(), 8) != salt1_ || Get<uint32_t>(frame.data(), 12) != salt2_)
                break; // left over from an older log generation
            uint64_t sum = Checksum(running_checksum_, frame.data(), 16);
            sum = Checksum(sum, frame.data() + FRAME_HEADER_SIZE, PAGE_SIZE);
            if (sum != Get<uint64_t>(frame.data(), 16))
                break; // torn or corrupt frame

            running_checksum_ = sum;
            pending_[static_cast<page_id_t>(Get<uint32_t>(frame.data(), 0))] = frame_count_;
            ++frame_count_;

            const uint32_t commit_page_count = Get<uint32_t>(frame.data(), 4);
            if (commit_page_count != 0)
            {
                for (const auto &entry : pending_)
                    committed_[entry.first] = entry.second;
                pending_.clear();
                committed_frames_ = frame_count_;
                committed_checksum_ = running_checksum_;
                committed_page_count_ = commit_page_count;
            }
        }

        // Frames after the last commit belong to a transaction that never
        // finished: drop them
        pending_.clear();
        frame_count_ = committed_frames_;
        running_checksum_ = committed_checksum_;
        if (ftruncate(fd_, static_cast<off_t>(FrameOffset(frame_count_))) != 0)
            return RecoverResult::IO_ERROR;
        return RecoverResult::RECOVERED;
    }

    bool WriteAheadLog::Reset()
    {
        std::random_device rd;
        salt1_ = rd();
        salt2_ = rd();

        std::array<char, HEADER_SIZE> header{};
        std::memcpy(header.data(), MAGIC, sizeof(MAGIC));
        Put<uint32_t>(header.data(), 8, VERSION);
        Put<uint32_t>(header.data(), 12, static_cast<uint32_t>(PAGE_SIZE));
        Put<uint32_t>(header.data(), 16, salt1_);
        Put<uint32_t>(header.data(), 20, salt2_);
        const uint64_t sum = Checksum(FNV_OFFSET, header.data(), 24);
        Put<uint64_t>(header.data(), 24, sum);

        if (ftruncate(fd_, 0) != 0 || !PwriteFull(fd_, header.data(), HEADER_SIZE, 0) || fsync(fd_) != 0)
            return false;

        committed_.clear();
        pending_.clear();
        frame_count_ = committed_frames_ = 0;
        running_checksum_ = committed_checksum_ = sum;
        committed_page_count_ = 0;
        return true;
    }

    bool WriteAheadLog::AppendFrame(page_id_t page_id, const char *data, uint32_t commit_page_count)
    {
        if (fd_ < 0)
            return false;

        std::array<char, FRAME_SIZE> frame{};
        Put<uint32_t>(frame.data(), 0, static_cast<uint32_t>(page_id));
        Put<uint32_t>(frame.data(), 4, commit_page_count);
        Put<uint32_t>(frame.data(), 8, salt1_);
        Put<uint32_t>(frame.data(), 12, salt2_);
        std::memcpy(frame.data() + FRAME_HEADER_SIZE, data, PAGE_SIZE);
        uint64_t sum = Checksum(running_checksum_, frame.data(), 16);
        sum = Checksum(sum, frame.data() + FRAME_HEADER_SIZE, PAGE_SIZE);
        Put<uint64_t>(frame.data(), 16, sum);

        if (!PwriteFull(fd_, frame.data(), FRAME_SIZE, FrameOffset(frame_count_)))
            return false;
        running_checksum_ = sum;
        pending_[page_id] = frame_count_;
        ++frame_count_;

        if (commit_page_count != 0)
        {
            if (fsync(fd_) != 0)
                return false;
            for (const auto &entry : pending_)
                committed_[entry.first] = entry.second;
            pending_.clear();
            committed_frames_ = frame_count_;
            committed_checksum_ = running_checksum_;
            committed_page_count_ = commit_page_count;
        }
        return true;
    }

    bool WriteAheadLog::Rollback()
    {
        if (fd_ < 0)
            return false;
        pending_.clear();
        frame_count_ = committed_frames_;
        running_checksum_ = committed_checksum_;
        return ftruncate(fd_, static_cast<off_t>(FrameOffset(frame_count_))) == 0;
    }

    bool WriteAheadLog::RollbackTo(const Savepoint &savepoint)
    {
        if (fd_ < 0 || savepoint.frame_count < committed_frames_ || savepoint.frame_count > frame_count_)
            return false;
        pending_ = savepoint.pending;
        frame_count_ = savepoint.frame_count;
        running_checksum_ = savepoint.checksum;
        return ftruncate(fd_, static_cast<off_t>(FrameOffset(frame_count_))) == 0;
    }

    bool WriteAheadLog::ReadFrameData(uint64_t frame_no, char *out) const
    {
        return fd_ >= 0 && frame_no < frame_count_ &&
               PreadFull(fd_, out, PAGE_SIZE, FrameOffset(frame_no) + FRAME_HEADER_SIZE);
    }

    bool WriteAheadLog::ReadPage(page_id_t page_id, char *out) const
    {
        auto it = pending_.find(page_id);
        if (it != pending_.end())
            return ReadFrameData(it->second, out);
        it = committed_.find(page_id);
        if (it != committed_.end())
            return ReadFrameData(it->second, out);
        return false;
    }

    bool WriteAheadLog::Sync()
    {
        return fd_ >= 0 && fsync(fd_) == 0;
    }

} // namespace sql
