#include <gtest/gtest.h>
#include "storage/buffer_pool.h"
#include "storage/pager.h"
#include "storage/table.h"
#include "storage/table_heap.h"
#include "storage/table_page.h"
#include <array>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

namespace sql
{

    // ── TablePage ─────────────────────────────────────────────────────────────

    class TablePageTest : public ::testing::Test
    {
    protected:
        void SetUp() override { page_.Init(); }

        std::array<char, PAGE_SIZE> buf_{};
        TablePage page_{buf_.data()};

        std::string Get(uint16_t slot)
        {
            const char *data;
            uint16_t size;
            if (!page_.GetTuple(slot, &data, &size))
                return "<none>";
            return std::string(data, size);
        }
    };

    TEST_F(TablePageTest, InsertAndGet)
    {
        uint16_t a, b;
        ASSERT_TRUE(page_.InsertTuple("alpha", 5, &a));
        ASSERT_TRUE(page_.InsertTuple("beta", 4, &b));
        EXPECT_EQ(a, 0);
        EXPECT_EQ(b, 1);
        EXPECT_EQ(Get(a), "alpha");
        EXPECT_EQ(Get(b), "beta");
        EXPECT_EQ(page_.GetNextPageId(), INVALID_PAGE_ID);
    }

    TEST_F(TablePageTest, DeleteLeavesOtherSlotsStable)
    {
        uint16_t a, b, c;
        page_.InsertTuple("a", 1, &a);
        page_.InsertTuple("b", 1, &b);
        page_.InsertTuple("c", 1, &c);
        ASSERT_TRUE(page_.DeleteTuple(b));
        EXPECT_FALSE(page_.DeleteTuple(b));
        EXPECT_EQ(Get(a), "a");
        EXPECT_EQ(Get(b), "<none>");
        EXPECT_EQ(Get(c), "c");
        EXPECT_EQ(page_.GetSlotCount(), 3);

        // Deleting the trailing slots trims the slot array
        page_.DeleteTuple(c);
        EXPECT_EQ(page_.GetSlotCount(), 1);
    }

    TEST_F(TablePageTest, FillsUpThenCompactsDeletedSpace)
    {
        const std::string row(100, 'r');
        std::vector<uint16_t> slots;
        uint16_t slot;
        while (page_.InsertTuple(row.data(), static_cast<uint16_t>(row.size()), &slot))
            slots.push_back(slot);
        ASSERT_GT(slots.size(), 30u);

        // Free every other tuple; a larger row must now fit via compaction
        for (size_t i = 0; i + 1 < slots.size(); i += 2)
            page_.DeleteTuple(slots[i]);
        const std::string big(150, 'B');
        ASSERT_TRUE(page_.InsertTuple(big.data(), static_cast<uint16_t>(big.size()), &slot));
        EXPECT_EQ(Get(slot), big);
        for (size_t i = 1; i < slots.size(); i += 2)
            EXPECT_EQ(Get(slots[i]), row) << i;
    }

    TEST_F(TablePageTest, UpdateInPlaceAndGrow)
    {
        uint16_t a, b;
        page_.InsertTuple("hello", 5, &a);
        page_.InsertTuple("world", 5, &b);

        ASSERT_TRUE(page_.UpdateTuple(a, "hi", 2));
        EXPECT_EQ(Get(a), "hi");
        ASSERT_TRUE(page_.UpdateTuple(a, "a much longer value", 19));
        EXPECT_EQ(Get(a), "a much longer value");
        EXPECT_EQ(Get(b), "world");
    }

    TEST_F(TablePageTest, UpdateFailsWhenPageCannotHoldNewVersion)
    {
        uint16_t slot;
        const std::string filler(TablePage::MAX_TUPLE_SIZE - 200, 'f');
        page_.InsertTuple(filler.data(), static_cast<uint16_t>(filler.size()), &slot);
        uint16_t small;
        page_.InsertTuple("x", 1, &small);
        const std::string huge(400, 'h');
        EXPECT_FALSE(page_.UpdateTuple(small, huge.data(), static_cast<uint16_t>(huge.size())));
        EXPECT_EQ(Get(small), "x");
    }

    TEST_F(TablePageTest, RejectsOversizedTuple)
    {
        std::string too_big(TablePage::MAX_TUPLE_SIZE + 1, 'x');
        uint16_t slot;
        EXPECT_FALSE(page_.InsertTuple(too_big.data(), static_cast<uint16_t>(too_big.size()), &slot));
        std::string max(TablePage::MAX_TUPLE_SIZE, 'x');
        EXPECT_TRUE(page_.InsertTuple(max.data(), static_cast<uint16_t>(max.size()), &slot));
    }

    // ── TableHeap / Table ─────────────────────────────────────────────────────

    class TableHeapTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            path_ = ::testing::TempDir() + "heap_test_" + std::to_string(getpid()) + "_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
            std::remove(path_.c_str());
        }
        void TearDown() override { std::remove(path_.c_str()); }

        static Tuple Row(int id, const std::string &name)
        {
            return Tuple({Value(id), Value(name)});
        }

        std::string path_;
    };

    TEST_F(TableHeapTest, ManyRowsSpanPagesWithTinyBufferPool)
    {
        Pager pager;
        pager.OpenInMemory();
        BufferPoolManager bpm(3, &pager);
        auto heap = TableHeap::Create(&bpm);

        const int n = 5000;
        std::vector<RID> rids;
        for (int i = 0; i < n; ++i)
            rids.push_back(heap->InsertTuple(Row(i, "name-" + std::to_string(i))));
        EXPECT_GT(pager.GetPageCount(), 20u); // far more pages than frames

        // Scan order matches insertion order
        RID rid;
        Tuple tuple;
        int expected = 0;
        for (bool ok = heap->FirstTuple(&rid, &tuple); ok; ok = heap->NextTuple(rid, &rid, &tuple))
        {
            ASSERT_EQ(tuple.GetValue(0).GetAsInt(), expected);
            ASSERT_EQ(rid, rids[static_cast<size_t>(expected)]);
            ++expected;
        }
        EXPECT_EQ(expected, n);

        // Point lookups by RID
        ASSERT_TRUE(heap->GetTuple(rids[1234], &tuple));
        EXPECT_EQ(tuple.GetValue(1).GetAsString(), "name-1234");
    }

    TEST_F(TableHeapTest, DeleteAndUpdateThroughTable)
    {
        Pager pager;
        pager.OpenInMemory();
        BufferPoolManager bpm(4, &pager);
        Table table("t", Schema({Column("id", DataType::INTEGER), Column("name", DataType::VARCHAR, 4000)}),
                    TableHeap::Create(&bpm));

        std::vector<RID> rids;
        for (int i = 0; i < 100; ++i)
            rids.push_back(table.Insert(Row(i, "x")));
        EXPECT_EQ(table.GetTupleCount(), 100u);

        for (int i = 0; i < 100; i += 2)
            ASSERT_TRUE(table.DeleteTuple(rids[static_cast<size_t>(i)]));
        EXPECT_FALSE(table.DeleteTuple(rids[0]));
        EXPECT_EQ(table.GetTupleCount(), 50u);

        // Grow one row beyond what its page can hold even after compaction
        // (100 slots + 50 live rows leave ~3KB): it must move to a new page
        RID moved;
        const std::string big(3800, 'b');
        ASSERT_TRUE(table.UpdateTuple(rids[1], Row(1, big), &moved));
        EXPECT_NE(moved, rids[1]);
        Tuple tuple;
        EXPECT_FALSE(table.GetTuple(rids[1], &tuple));
        ASSERT_TRUE(table.GetTuple(moved, &tuple));
        EXPECT_EQ(tuple.GetValue(1).GetAsString(), big);
        EXPECT_EQ(table.GetTupleCount(), 50u);

        size_t seen = 0;
        for (const auto &row : table)
        {
            EXPECT_EQ(row.GetValue(0).GetAsInt() % 2, 1);
            ++seen;
        }
        EXPECT_EQ(seen, 50u);
    }

    TEST_F(TableHeapTest, OversizedRowThrows)
    {
        Pager pager;
        pager.OpenInMemory();
        BufferPoolManager bpm(4, &pager);
        auto heap = TableHeap::Create(&bpm);
        EXPECT_THROW(heap->InsertTuple(Row(1, std::string(PAGE_SIZE, 'x'))), std::runtime_error);
    }

    TEST_F(TableHeapTest, DropReturnsPagesToFreeList)
    {
        Pager pager;
        pager.OpenInMemory();
        BufferPoolManager bpm(4, &pager);
        auto heap = TableHeap::Create(&bpm);
        for (int i = 0; i < 2000; ++i)
            heap->InsertTuple(Row(i, "row"));
        const uint32_t pages_before = pager.GetPageCount();

        heap->Drop();
        EXPECT_NE(pager.GetFreeListHead(), INVALID_PAGE_ID);

        // A new heap reuses freed pages instead of growing the file
        auto again = TableHeap::Create(&bpm);
        for (int i = 0; i < 2000; ++i)
            again->InsertTuple(Row(i, "row"));
        EXPECT_EQ(pager.GetPageCount(), pages_before);
    }

    TEST_F(TableHeapTest, HeapSurvivesReopenFromFile)
    {
        page_id_t first_page;
        {
            Pager pager;
            ASSERT_TRUE(pager.Open(path_));
            BufferPoolManager bpm(4, &pager);
            auto heap = TableHeap::Create(&bpm);
            first_page = heap->GetFirstPageId();
            for (int i = 0; i < 1500; ++i)
                heap->InsertTuple(Row(i, "persisted-" + std::to_string(i)));
        }

        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        BufferPoolManager bpm(4, &pager);
        Table table("t", Schema({Column("id", DataType::INTEGER), Column("name", DataType::VARCHAR, 50)}),
                    std::make_unique<TableHeap>(&bpm, first_page));
        EXPECT_EQ(table.GetTupleCount(), 1500u);

        int expected = 0;
        for (const auto &row : table)
        {
            ASSERT_EQ(row.GetValue(0).GetAsInt(), expected);
            ASSERT_EQ(row.GetValue(1).GetAsString(), "persisted-" + std::to_string(expected));
            ++expected;
        }

        // Appends continue on the existing chain
        table.Insert(Row(9999, "tail"));
        EXPECT_EQ(table.GetTupleCount(), 1501u);
    }

    TEST_F(TableHeapTest, TemporaryTableIteratesAndDeletes)
    {
        Table temp("tmp", Schema({Column("id", DataType::INTEGER)}));
        EXPECT_TRUE(temp.IsTemporary());
        RID a = temp.Insert(Tuple({Value(1)}));
        temp.Insert(Tuple({Value(2)}));
        temp.Insert(Tuple({Value(3)}));
        ASSERT_TRUE(temp.DeleteTuple(a));

        std::vector<int> ids;
        for (const auto &row : temp)
            ids.push_back(row.GetValue(0).GetAsInt());
        EXPECT_EQ(ids, (std::vector<int>{2, 3}));
        EXPECT_EQ(temp.GetTupleCount(), 2u);
    }

} // namespace sql
