#include "cluster/shard/file_log_store.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace sql::cluster
{

    using nuraft::buffer;
    using nuraft::log_entry;
    using nuraft::ptr;
    using nuraft::ulong;

    namespace
    {
        std::runtime_error IoError(const std::string &what, const std::string &path)
        {
            return std::runtime_error(what + " " + path + ": " + std::strerror(errno));
        }

        void WriteAll(int fd, const char *data, size_t size, uint64_t offset, const std::string &path)
        {
            while (size > 0)
            {
                const ssize_t n = ::pwrite(fd, data, size, static_cast<off_t>(offset));
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    throw IoError("cannot write", path);
                }
                data += n;
                size -= static_cast<size_t>(n);
                offset += static_cast<uint64_t>(n);
            }
        }

        ptr<log_entry> Clone(const ptr<log_entry> &entry)
        {
            return nuraft::cs_new<log_entry>(entry->get_term(), buffer::clone(entry->get_buf()),
                                             entry->get_val_type(), entry->get_timestamp());
        }

        void SyncDirectory(const std::string &dir)
        {
            const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
            if (fd >= 0)
            {
                ::fsync(fd);
                ::close(fd);
            }
        }

        constexpr char MAGIC[8] = {'S', 'Q', 'L', 'R', 'L', 'O', 'G', '1'};
        constexpr uint64_t HEADER_SIZE = 16;

        std::string Header(ulong start)
        {
            std::string header(MAGIC, sizeof(MAGIC));
            header.append(reinterpret_cast<const char *>(&start), sizeof(start));
            return header;
        }

        std::string Record(const ptr<log_entry> &entry)
        {
            ptr<buffer> body = entry->serialize();
            const uint32_t size = static_cast<uint32_t>(body->size());
            const uint32_t crc = Crc32(body->data_begin(), body->size());
            std::string record(8 + size, '\0');
            std::memcpy(record.data(), &size, 4);
            std::memcpy(record.data() + 4, &crc, 4);
            std::memcpy(record.data() + 8, body->data_begin(), size);
            return record;
        }
    } // namespace

    uint32_t Crc32(const void *data, size_t size)
    {
        static const auto table = []
        {
            std::vector<uint32_t> t(256);
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        uint32_t crc = 0xFFFFFFFFu;
        const auto *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    void WriteFileAtomically(const std::string &path, const std::string &contents)
    {
        const std::string tmp = path + ".tmp";
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            throw IoError("cannot create", tmp);
        try
        {
            WriteAll(fd, contents.data(), contents.size(), 0, tmp);
        }
        catch (...)
        {
            ::close(fd);
            throw;
        }
        if (::fsync(fd) != 0)
        {
            ::close(fd);
            throw IoError("cannot sync", tmp);
        }
        ::close(fd);
        if (::rename(tmp.c_str(), path.c_str()) != 0)
            throw IoError("cannot replace", path);
        const size_t slash = path.rfind('/');
        SyncDirectory(slash == std::string::npos ? "." : path.substr(0, slash));
    }

    bool ReadFile(const std::string &path, std::string *contents)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        std::ostringstream out;
        out << file.rdbuf();
        *contents = out.str();
        return true;
    }

    FileLogStore::FileLogStore(const std::string &directory)
        : dir_(directory), dummy_(nuraft::cs_new<log_entry>(0, buffer::alloc(sizeof(ulong))))
    {
        const std::string path = dir_ + "/log";
        std::string bytes;
        if (!ReadFile(path, &bytes) || bytes.empty())
        {
            RewriteFile(); // a new, empty log starting at 1
            return;
        }
        if (bytes.size() < HEADER_SIZE || std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0)
            throw std::runtime_error(path + " is not a Raft log");
        std::memcpy(&start_, bytes.data() + sizeof(MAGIC), sizeof(start_));
        fd_ = ::open(path.c_str(), O_RDWR, 0644);
        if (fd_ < 0)
            throw IoError("cannot open", path);

        // Keep every whole, intact record; cut the file after the last one
        size_t pos = HEADER_SIZE;
        while (bytes.size() - pos >= 8)
        {
            uint32_t size, crc;
            std::memcpy(&size, bytes.data() + pos, 4);
            std::memcpy(&crc, bytes.data() + pos + 4, 4);
            if (bytes.size() - pos - 8 < size || Crc32(bytes.data() + pos + 8, size) != crc)
                break;
            ptr<buffer> body = buffer::alloc(size);
            std::memcpy(body->data_begin(), bytes.data() + pos + 8, size);
            entries_.push_back(log_entry::deserialize(*body));
            offsets_.push_back(pos);
            pos += 8 + size;
        }
        end_ = pos;
        if (pos < bytes.size())
        {
            dropped_on_open_ = 1; // the torn record and whatever followed it
            if (::ftruncate(fd_, static_cast<off_t>(end_)) != 0)
                throw IoError("cannot truncate", path);
            Sync();
        }
    }

    FileLogStore::~FileLogStore()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    ptr<log_entry> FileLogStore::At(ulong index) const
    {
        if (index < start_ || index >= start_ + entries_.size())
            return dummy_;
        return entries_[index - start_];
    }

    void FileLogStore::AppendRecord(const ptr<log_entry> &entry)
    {
        const std::string record = Record(entry);
        WriteAll(fd_, record.data(), record.size(), end_, dir_ + "/log");
        offsets_.push_back(end_);
        end_ += record.size();
        entries_.push_back(Clone(entry));
    }

    void FileLogStore::TruncateFrom(ulong index)
    {
        if (index < start_)
            index = start_;
        const size_t keep = static_cast<size_t>(index - start_);
        if (keep >= entries_.size())
            return;
        end_ = offsets_[keep];
        entries_.resize(keep);
        offsets_.resize(keep);
        if (::ftruncate(fd_, static_cast<off_t>(end_)) != 0)
            throw IoError("cannot truncate", dir_ + "/log");
    }

    void FileLogStore::Sync()
    {
        if (::fdatasync(fd_) != 0)
            throw IoError("cannot sync", dir_ + "/log");
    }

    void FileLogStore::RewriteFile()
    {
        std::string bytes = Header(start_);
        offsets_.clear();
        for (const auto &entry : entries_)
        {
            offsets_.push_back(bytes.size());
            bytes += Record(entry);
        }
        WriteFileAtomically(dir_ + "/log", bytes);
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = ::open((dir_ + "/log").c_str(), O_RDWR, 0644);
        if (fd_ < 0)
            throw IoError("cannot reopen", dir_ + "/log");
        end_ = bytes.size();
    }

    ulong FileLogStore::next_slot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_ + entries_.size();
    }

    ulong FileLogStore::start_index() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_;
    }

    ptr<log_entry> FileLogStore::last_entry() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return Clone(entries_.empty() ? dummy_ : entries_.back());
    }

    ulong FileLogStore::append(ptr<log_entry> &entry)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AppendRecord(entry);
        return start_ + entries_.size() - 1;
    }

    void FileLogStore::write_at(ulong index, ptr<log_entry> &entry)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < start_ || index > start_ + entries_.size())
        {
            // Not adjacent to what we hold: the log restarts here
            entries_.clear();
            start_ = index;
            RewriteFile();
        }
        else
            TruncateFrom(index);
        AppendRecord(entry);
        Sync();
    }

    void FileLogStore::end_of_append_batch(ulong, ulong)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Sync();
    }

    ptr<std::vector<ptr<log_entry>>> FileLogStore::log_entries(ulong start, ulong end)
    {
        auto result = nuraft::cs_new<std::vector<ptr<log_entry>>>();
        std::lock_guard<std::mutex> lock(mutex_);
        for (ulong i = start; i < end; ++i)
            result->push_back(Clone(At(i)));
        return result;
    }

    ptr<log_entry> FileLogStore::entry_at(ulong index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return Clone(At(index));
    }

    ulong FileLogStore::term_at(ulong index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return At(index)->get_term();
    }

    ptr<buffer> FileLogStore::pack(ulong index, nuraft::int32 count)
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

    void FileLogStore::apply_pack(ulong index, buffer &pack)
    {
        pack.pos(0);
        const nuraft::int32 count = pack.get_int();
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < start_ || index > start_ + entries_.size())
        {
            entries_.clear();
            start_ = index;
            RewriteFile();
        }
        else
            TruncateFrom(index);
        for (nuraft::int32 i = 0; i < count; ++i)
        {
            const nuraft::int32 size = pack.get_int();
            ptr<buffer> entry = buffer::alloc(size);
            pack.get(entry);
            AppendRecord(log_entry::deserialize(*entry));
        }
        Sync();
    }

    bool FileLogStore::compact(ulong last_log_index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_log_index < start_)
            return true;
        const ulong drop = last_log_index - start_ + 1;
        if (drop >= entries_.size())
            entries_.clear();
        else
            entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(drop));
        start_ = last_log_index + 1;
        RewriteFile();
        return true;
    }

    bool FileLogStore::flush()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Sync();
        return true;
    }

} // namespace sql::cluster
