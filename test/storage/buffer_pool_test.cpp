#include <gtest/gtest.h>
#include "storage/buffer_pool.h"
#include "storage/pager.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

namespace sql
{

    class BufferPoolTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            path_ = ::testing::TempDir() + "bpm_test_" + std::to_string(getpid()) + "_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
            std::remove(path_.c_str());
        }
        void TearDown() override { std::remove(path_.c_str()); }

        std::string path_;
    };

    // ── Pager ─────────────────────────────────────────────────────────────────

    TEST_F(BufferPoolTest, PagerCreatesHeader)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        EXPECT_EQ(pager.GetPageCount(), 1u);
        EXPECT_EQ(pager.GetFreeListHead(), INVALID_PAGE_ID);
        EXPECT_EQ(pager.GetCatalogRoot(), INVALID_PAGE_ID);
    }

    TEST_F(BufferPoolTest, PagerRejectsForeignFile)
    {
        FILE *f = std::fopen(path_.c_str(), "wb");
        std::string junk(PAGE_SIZE, 'x');
        std::fwrite(junk.data(), 1, junk.size(), f);
        std::fclose(f);

        Pager pager;
        EXPECT_FALSE(pager.Open(path_));
    }

    TEST_F(BufferPoolTest, PagerHeaderPersists)
    {
        {
            Pager pager;
            ASSERT_TRUE(pager.Open(path_));
            EXPECT_EQ(pager.AllocatePage(), 1);
            EXPECT_EQ(pager.AllocatePage(), 2);
            ASSERT_TRUE(pager.SetCatalogRoot(2));
        }
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        EXPECT_EQ(pager.GetPageCount(), 3u);
        EXPECT_EQ(pager.GetCatalogRoot(), 2);
    }

    TEST_F(BufferPoolTest, PagerFreeListReusesPages)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        page_id_t a = pager.AllocatePage();
        page_id_t b = pager.AllocatePage();
        ASSERT_TRUE(pager.DeallocatePage(a));
        ASSERT_TRUE(pager.DeallocatePage(b));
        EXPECT_EQ(pager.AllocatePage(), b); // LIFO
        EXPECT_EQ(pager.AllocatePage(), a);
        EXPECT_EQ(pager.GetFreeListHead(), INVALID_PAGE_ID);
        EXPECT_EQ(pager.GetPageCount(), 3u);
    }

    TEST_F(BufferPoolTest, PagerRejectsHeaderAndOutOfRangeIO)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        char buf[PAGE_SIZE] = {};
        EXPECT_FALSE(pager.ReadPage(0, buf));
        EXPECT_FALSE(pager.WritePage(0, buf));
        EXPECT_FALSE(pager.ReadPage(5, buf));
    }

    // ── BufferPoolManager ─────────────────────────────────────────────────────

    TEST_F(BufferPoolTest, NewPageIsZeroedAndPinned)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        BufferPoolManager bpm(4, &pager);

        page_id_t pid;
        Page *page = bpm.NewPage(&pid);
        ASSERT_NE(page, nullptr);
        EXPECT_EQ(page->GetPageId(), pid);
        EXPECT_EQ(page->GetPinCount(), 1);
        for (size_t i = 0; i < PAGE_SIZE; ++i)
            ASSERT_EQ(page->GetData()[i], 0);
        EXPECT_TRUE(bpm.UnpinPage(pid, false));
        EXPECT_FALSE(bpm.UnpinPage(pid, false)); // already unpinned
    }

    TEST_F(BufferPoolTest, AllPinnedReturnsNull)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        BufferPoolManager bpm(2, &pager);

        page_id_t p1, p2, p3;
        ASSERT_NE(bpm.NewPage(&p1), nullptr);
        ASSERT_NE(bpm.NewPage(&p2), nullptr);
        EXPECT_EQ(bpm.NewPage(&p3), nullptr);

        bpm.UnpinPage(p1, false);
        EXPECT_NE(bpm.NewPage(&p3), nullptr);
    }

    TEST_F(BufferPoolTest, EvictionWritesBackDirtyPages)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        BufferPoolManager bpm(3, &pager);

        // Write more pages than frames so earlier ones are evicted
        std::vector<page_id_t> ids;
        for (int i = 0; i < 10; ++i)
        {
            page_id_t pid;
            Page *page = bpm.NewPage(&pid);
            ASSERT_NE(page, nullptr);
            std::snprintf(page->GetData(), PAGE_SIZE, "page-%d", i);
            page->Write<int32_t>(PAGE_SIZE - 4, i * 7);
            ASSERT_TRUE(bpm.UnpinPage(pid, true));
            ids.push_back(pid);
        }

        for (int i = 0; i < 10; ++i)
        {
            Page *page = bpm.FetchPage(ids[i]);
            ASSERT_NE(page, nullptr);
            EXPECT_EQ(std::string(page->GetData()), "page-" + std::to_string(i));
            EXPECT_EQ(page->Read<int32_t>(PAGE_SIZE - 4), i * 7);
            bpm.UnpinPage(ids[i], false);
        }
    }

    TEST_F(BufferPoolTest, LruEvictsLeastRecentlyUnpinned)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        BufferPoolManager bpm(2, &pager);

        page_id_t a, b, c;
        bpm.NewPage(&a);
        bpm.NewPage(&b);
        bpm.UnpinPage(a, true);
        bpm.UnpinPage(b, true);

        // Touch a so b becomes the LRU victim
        ASSERT_NE(bpm.FetchPage(a), nullptr);
        bpm.UnpinPage(a, false);

        ASSERT_NE(bpm.NewPage(&c), nullptr); // evicts b
        // a is still resident: pinning it plus c fills the pool, so fetching b must fail
        ASSERT_NE(bpm.FetchPage(a), nullptr);
        EXPECT_EQ(bpm.FetchPage(b), nullptr);
    }

    TEST_F(BufferPoolTest, DataSurvivesReopen)
    {
        page_id_t pid;
        {
            Pager pager;
            ASSERT_TRUE(pager.Open(path_));
            BufferPoolManager bpm(4, &pager);
            PageGuard guard = bpm.NewPageGuarded(&pid);
            ASSERT_TRUE(guard);
            std::strcpy(guard.GetData(), "persisted");
            guard.MarkDirty();
        } // guard unpins, bpm destructor flushes

        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        BufferPoolManager bpm(4, &pager);
        PageGuard guard = bpm.FetchPageGuarded(pid);
        ASSERT_TRUE(guard);
        EXPECT_STREQ(guard.GetData(), "persisted");
    }

    TEST_F(BufferPoolTest, DeletePageRequiresUnpinned)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        BufferPoolManager bpm(4, &pager);

        page_id_t pid;
        bpm.NewPage(&pid);
        EXPECT_FALSE(bpm.DeletePage(pid));
        bpm.UnpinPage(pid, false);
        EXPECT_TRUE(bpm.DeletePage(pid));
        EXPECT_EQ(pager.GetFreeListHead(), pid);

        page_id_t reused;
        ASSERT_NE(bpm.NewPage(&reused), nullptr);
        EXPECT_EQ(reused, pid);
    }

    TEST_F(BufferPoolTest, PageGuardMoveTransfersPin)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        BufferPoolManager bpm(1, &pager);

        page_id_t pid;
        PageGuard outer;
        {
            PageGuard inner = bpm.NewPageGuarded(&pid);
            ASSERT_TRUE(inner);
            outer = std::move(inner);
            EXPECT_FALSE(inner);
        }
        EXPECT_EQ(outer.GetPage()->GetPinCount(), 1);
        page_id_t other;
        EXPECT_EQ(bpm.NewPage(&other), nullptr); // still pinned by outer
        outer.Release();
        EXPECT_NE(bpm.NewPage(&other), nullptr);
    }

} // namespace sql
