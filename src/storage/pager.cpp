#include "storage/pager.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cstring>

namespace sql
{

    namespace
    {
        constexpr char MAGIC[8] = {'S', 'Q', 'L', 'E', 'N', 'G', '0', '1'};
        constexpr size_t OFF_VERSION = 8;
        constexpr size_t OFF_PAGE_COUNT = 12;
        constexpr size_t OFF_FREE_HEAD = 16;
        constexpr size_t OFF_CATALOG_ROOT = 20;

        bool PreadFull(int fd, char *buf, size_t len, off_t offset)
        {
            size_t done = 0;
            while (done < len)
            {
                ssize_t n = pread(fd, buf + done, len - done, offset + static_cast<off_t>(done));
                if (n <= 0)
                    return false;
                done += static_cast<size_t>(n);
            }
            return true;
        }

        bool PwriteFull(int fd, const char *buf, size_t len, off_t offset)
        {
            size_t done = 0;
            while (done < len)
            {
                ssize_t n = pwrite(fd, buf + done, len - done, offset + static_cast<off_t>(done));
                if (n <= 0)
                    return false;
                done += static_cast<size_t>(n);
            }
            return true;
        }

        off_t PageOffset(page_id_t page_id)
        {
            return static_cast<off_t>(page_id) * static_cast<off_t>(PAGE_SIZE);
        }
    } // namespace

    Pager::~Pager()
    {
        Close();
    }

    bool Pager::Open(const std::string &path)
    {
        Close();
        fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ < 0)
            return false;

        struct stat st;
        if (fstat(fd_, &st) != 0)
        {
            Close();
            return false;
        }

        if (st.st_size == 0)
        {
            page_count_ = 1;
            free_list_head_ = INVALID_PAGE_ID;
            catalog_root_ = INVALID_PAGE_ID;
            if (!WriteHeader())
            {
                Close();
                return false;
            }
            return true;
        }

        if (!ReadHeader())
        {
            Close();
            return false;
        }
        return true;
    }

    void Pager::Close()
    {
        if (fd_ >= 0)
        {
            fsync(fd_);
            close(fd_);
            fd_ = -1;
        }
    }

    bool Pager::ReadHeader()
    {
        std::array<char, PAGE_SIZE> buf{};
        if (!PreadFull(fd_, buf.data(), PAGE_SIZE, 0))
            return false;
        if (std::memcmp(buf.data(), MAGIC, sizeof(MAGIC)) != 0)
            return false;

        uint32_t version;
        std::memcpy(&version, buf.data() + OFF_VERSION, sizeof(version));
        if (version != FORMAT_VERSION)
            return false;

        std::memcpy(&page_count_, buf.data() + OFF_PAGE_COUNT, sizeof(page_count_));
        std::memcpy(&free_list_head_, buf.data() + OFF_FREE_HEAD, sizeof(free_list_head_));
        std::memcpy(&catalog_root_, buf.data() + OFF_CATALOG_ROOT, sizeof(catalog_root_));
        return page_count_ >= 1;
    }

    bool Pager::WriteHeader()
    {
        std::array<char, PAGE_SIZE> buf{};
        std::memcpy(buf.data(), MAGIC, sizeof(MAGIC));
        uint32_t version = FORMAT_VERSION;
        std::memcpy(buf.data() + OFF_VERSION, &version, sizeof(version));
        std::memcpy(buf.data() + OFF_PAGE_COUNT, &page_count_, sizeof(page_count_));
        std::memcpy(buf.data() + OFF_FREE_HEAD, &free_list_head_, sizeof(free_list_head_));
        std::memcpy(buf.data() + OFF_CATALOG_ROOT, &catalog_root_, sizeof(catalog_root_));
        return PwriteFull(fd_, buf.data(), PAGE_SIZE, 0);
    }

    bool Pager::ReadPage(page_id_t page_id, char *out)
    {
        if (fd_ < 0 || page_id <= 0 || static_cast<uint32_t>(page_id) >= page_count_)
            return false;
        return PreadFull(fd_, out, PAGE_SIZE, PageOffset(page_id));
    }

    bool Pager::WritePage(page_id_t page_id, const char *data)
    {
        if (fd_ < 0 || page_id <= 0 || static_cast<uint32_t>(page_id) >= page_count_)
            return false;
        return PwriteFull(fd_, data, PAGE_SIZE, PageOffset(page_id));
    }

    page_id_t Pager::AllocatePage()
    {
        if (fd_ < 0)
            return INVALID_PAGE_ID;

        std::array<char, PAGE_SIZE> zeros{};

        if (free_list_head_ != INVALID_PAGE_ID)
        {
            page_id_t page_id = free_list_head_;
            std::array<char, PAGE_SIZE> buf{};
            if (!PreadFull(fd_, buf.data(), PAGE_SIZE, PageOffset(page_id)))
                return INVALID_PAGE_ID;
            page_id_t next;
            std::memcpy(&next, buf.data(), sizeof(next));
            free_list_head_ = next;
            if (!PwriteFull(fd_, zeros.data(), PAGE_SIZE, PageOffset(page_id)) || !WriteHeader())
                return INVALID_PAGE_ID;
            return page_id;
        }

        page_id_t page_id = static_cast<page_id_t>(page_count_);
        page_count_++;
        if (!PwriteFull(fd_, zeros.data(), PAGE_SIZE, PageOffset(page_id)) || !WriteHeader())
        {
            page_count_--;
            return INVALID_PAGE_ID;
        }
        return page_id;
    }

    bool Pager::DeallocatePage(page_id_t page_id)
    {
        if (fd_ < 0 || page_id <= 0 || static_cast<uint32_t>(page_id) >= page_count_)
            return false;

        std::array<char, PAGE_SIZE> buf{};
        std::memcpy(buf.data(), &free_list_head_, sizeof(free_list_head_));
        if (!PwriteFull(fd_, buf.data(), PAGE_SIZE, PageOffset(page_id)))
            return false;
        free_list_head_ = page_id;
        return WriteHeader();
    }

    bool Pager::SetCatalogRoot(page_id_t page_id)
    {
        catalog_root_ = page_id;
        return WriteHeader();
    }

    bool Pager::Sync()
    {
        return fd_ >= 0 && fsync(fd_) == 0;
    }

} // namespace sql
