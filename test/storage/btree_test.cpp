#include <gtest/gtest.h>
#include "storage/btree.h"
#include "storage/buffer_pool.h"
#include "storage/pager.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace sql
{

    class BTreeTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            pager_.OpenInMemory();
            bpm_ = std::make_unique<BufferPoolManager>(8, &pager_);
        }

        static RID R(int i) { return RID(1000 + i / 100, static_cast<uint32_t>(i % 100)); }

        static std::vector<int> ToInts(const std::vector<RID> &rids)
        {
            std::vector<int> out;
            for (const auto &rid : rids)
                out.push_back((rid.page_id - 1000) * 100 + static_cast<int>(rid.slot));
            return out;
        }

        Pager pager_;
        std::unique_ptr<BufferPoolManager> bpm_;
    };

    TEST_F(BTreeTest, EmptyTree)
    {
        auto tree = BTree::Create(bpm_.get());
        EXPECT_TRUE(tree->IsEmpty());
        EXPECT_EQ(tree->GetHeight(), 1);
        EXPECT_TRUE(tree->Search(Value(1)).empty());
        EXPECT_TRUE(tree->RangeScan(std::nullopt, true, std::nullopt, true).empty());
        EXPECT_FALSE(tree->Remove(Value(1), R(1)));
    }

    TEST_F(BTreeTest, RandomInsertsSearchAndOrder)
    {
        auto tree = BTree::Create(bpm_.get());
        const page_id_t root = tree->GetRootPageId();

        std::vector<int> keys(4000);
        for (int i = 0; i < 4000; ++i)
            keys[static_cast<size_t>(i)] = i;
        std::mt19937 rng(42);
        std::shuffle(keys.begin(), keys.end(), rng);
        for (int k : keys)
            ASSERT_TRUE(tree->Insert(Value(k), R(k)));

        EXPECT_GE(tree->GetHeight(), 2);
        EXPECT_EQ(tree->GetRootPageId(), root); // root page id is stable

        for (int k = 0; k < 4000; k += 37)
            ASSERT_EQ(ToInts(tree->Search(Value(k))), std::vector<int>{k}) << k;
        EXPECT_TRUE(tree->Search(Value(4001)).empty());

        auto all = tree->GetAllEntries();
        ASSERT_EQ(all.size(), 4000u);
        for (size_t i = 0; i < all.size(); ++i)
            ASSERT_EQ(all[i].key.GetAsInt(), static_cast<int>(i));
    }

    TEST_F(BTreeTest, WideKeysSplitInternalNodes)
    {
        // ~200-byte keys: ~19 entries per node, so 3000 keys need 3+ levels
        auto tree = BTree::Create(bpm_.get());
        const page_id_t root = tree->GetRootPageId();
        auto key = [](int i)
        {
            std::string k = std::to_string(i);
            return Value(std::string(6 - k.size(), '0') + k + std::string(194, '.'));
        };
        std::vector<int> order(3000);
        for (int i = 0; i < 3000; ++i)
            order[static_cast<size_t>(i)] = i;
        std::shuffle(order.begin(), order.end(), std::mt19937(7));
        for (int i : order)
            ASSERT_TRUE(tree->Insert(key(i), R(i)));

        EXPECT_GE(tree->GetHeight(), 3);
        EXPECT_EQ(tree->GetRootPageId(), root);
        for (int i = 0; i < 3000; i += 11)
            ASSERT_EQ(ToInts(tree->Search(key(i))), std::vector<int>{i}) << i;
        EXPECT_EQ(tree->RangeScan(key(1000), true, key(1999), true).size(), 1000u);

        for (int i = 0; i < 3000; i += 2)
            ASSERT_TRUE(tree->Remove(key(i), R(i)));
        EXPECT_EQ(tree->GetAllEntries().size(), 1500u);
        EXPECT_TRUE(tree->Search(key(10)).empty());
        EXPECT_EQ(tree->Search(key(11)).size(), 1u);
    }

    TEST_F(BTreeTest, RangeScanBounds)
    {
        auto tree = BTree::Create(bpm_.get());
        for (int k = 0; k < 2000; ++k)
            tree->Insert(Value(k), R(k));

        EXPECT_EQ(tree->RangeScan(Value(100), true, Value(199), true).size(), 100u);
        EXPECT_EQ(tree->RangeScan(Value(100), false, Value(199), false).size(), 98u);
        EXPECT_EQ(tree->RangeScan(std::nullopt, true, Value(9), true).size(), 10u);
        EXPECT_EQ(tree->RangeScan(Value(1990), false, std::nullopt, true).size(), 9u);
        auto slice = ToInts(tree->RangeScan(Value(1500), true, Value(1504), true));
        EXPECT_EQ(slice, (std::vector<int>{1500, 1501, 1502, 1503, 1504}));
    }

    TEST_F(BTreeTest, DuplicateKeysAcrossManyLeaves)
    {
        auto tree = BTree::Create(bpm_.get());
        // 1500 entries share one key and span several leaves
        for (int i = 0; i < 1500; ++i)
        {
            tree->Insert(Value(7), R(i));
            tree->Insert(Value(i % 2 == 0 ? 6 : 8), R(i));
        }
        EXPECT_FALSE(tree->Insert(Value(7), R(5))); // exact pair already present

        auto sevens = ToInts(tree->Search(Value(7)));
        ASSERT_EQ(sevens.size(), 1500u);
        EXPECT_TRUE(std::is_sorted(sevens.begin(), sevens.end()));
        EXPECT_EQ(tree->Search(Value(6)).size(), 750u);
        EXPECT_EQ(tree->Search(Value(8)).size(), 750u);

        // Remove exactly one row's entry
        ASSERT_TRUE(tree->Remove(Value(7), R(1234)));
        EXPECT_FALSE(tree->Remove(Value(7), R(1234)));
        EXPECT_FALSE(tree->Remove(Value(6), R(1)));
        auto after = ToInts(tree->Search(Value(7)));
        EXPECT_EQ(after.size(), 1499u);
        EXPECT_EQ(std::find(after.begin(), after.end(), 1234), after.end());
    }

    TEST_F(BTreeTest, RemoveHalfThenScanSkipsEmptyLeaves)
    {
        auto tree = BTree::Create(bpm_.get());
        for (int k = 0; k < 2400; ++k)
            tree->Insert(Value(k), R(k));
        for (int k = 400; k < 1600; ++k)
            ASSERT_TRUE(tree->Remove(Value(k), R(k)));

        EXPECT_TRUE(tree->Search(Value(1000)).empty());
        EXPECT_EQ(tree->RangeScan(Value(200), true, Value(1800), false).size(), 400u);
        EXPECT_EQ(tree->GetAllEntries().size(), 1200u);

        // Re-inserting into the emptied range works
        for (int k = 400; k < 1600; ++k)
            ASSERT_TRUE(tree->Insert(Value(k), R(k)));
        EXPECT_EQ(tree->GetAllEntries().size(), 2400u);
    }

    TEST_F(BTreeTest, VarcharKeysOfMixedLengths)
    {
        auto tree = BTree::Create(bpm_.get());
        std::vector<std::string> keys;
        for (int i = 0; i < 400; ++i)
            keys.push_back(std::string(static_cast<size_t>(1 + (i * 37) % 900), static_cast<char>('a' + i % 26)) +
                           std::to_string(i));
        for (size_t i = 0; i < keys.size(); ++i)
            ASSERT_TRUE(tree->Insert(Value(keys[i]), R(static_cast<int>(i))));

        for (size_t i = 0; i < keys.size(); i += 7)
            ASSERT_EQ(ToInts(tree->Search(Value(keys[i]))), std::vector<int>{static_cast<int>(i)});

        auto all = tree->GetAllEntries();
        ASSERT_EQ(all.size(), keys.size());
        for (size_t i = 1; i < all.size(); ++i)
            ASSERT_LT(BTree::CompareKeys(all[i - 1].key, all[i].key), 1);
    }

    TEST_F(BTreeTest, RejectsUnindexableKeys)
    {
        auto tree = BTree::Create(bpm_.get());
        EXPECT_THROW(tree->Insert(Value(DataType::INTEGER), R(1)), std::invalid_argument);
        EXPECT_THROW(tree->Insert(Value(std::string(BTree::MAX_KEY_SIZE, 'x')), R(1)), std::invalid_argument);
        // Largest allowed key: tag + 4-byte length + payload
        EXPECT_TRUE(tree->Insert(Value(std::string(BTree::MAX_KEY_SIZE - 5, 'x')), R(1)));
        for (int i = 2; i < 30; ++i)
            ASSERT_TRUE(tree->Insert(Value(std::string(BTree::MAX_KEY_SIZE - 5, static_cast<char>('a' + i % 26))), R(i)));
        EXPECT_EQ(tree->GetAllEntries().size(), 29u);
    }

    TEST_F(BTreeTest, MixedTypesHaveATotalOrder)
    {
        auto tree = BTree::Create(bpm_.get());
        tree->Insert(Value(5), R(1));
        tree->Insert(Value("5"), R(2));
        tree->Insert(Value(5.0), R(3));
        tree->Insert(Value(true), R(4));
        EXPECT_EQ(ToInts(tree->Search(Value(5))), std::vector<int>{1});
        EXPECT_EQ(ToInts(tree->Search(Value("5"))), std::vector<int>{2});
        EXPECT_EQ(tree->GetAllEntries().size(), 4u);
    }

    TEST_F(BTreeTest, DropReturnsPagesToFreeList)
    {
        auto tree = BTree::Create(bpm_.get());
        for (int k = 0; k < 2000; ++k)
            tree->Insert(Value(k), R(k));
        const uint32_t pages = pager_.GetPageCount();
        tree->Drop();

        auto again = BTree::Create(bpm_.get());
        for (int k = 0; k < 2000; ++k)
            again->Insert(Value(k), R(k));
        EXPECT_EQ(pager_.GetPageCount(), pages);
    }

    TEST_F(BTreeTest, DropRefusesWhileAPageIsPinned)
    {
        auto tree = BTree::Create(bpm_.get());
        for (int k = 0; k < 1000; ++k)
            tree->Insert(Value(k), R(k));
        {
            PageGuard pinned = bpm_->FetchPageGuarded(tree->GetRootPageId());
            EXPECT_THROW(tree->Drop(), std::runtime_error);
            EXPECT_EQ(pager_.GetFreeListHead(), INVALID_PAGE_ID); // nothing freed
        }
        EXPECT_EQ(tree->Search(Value(500)).size(), 1u); // tree still intact
        tree->Drop();
        EXPECT_NE(pager_.GetFreeListHead(), INVALID_PAGE_ID);
    }

    TEST(BTreeFileTest, TreeSurvivesReopen)
    {
        const std::string path = ::testing::TempDir() + "btree_reopen_" + std::to_string(getpid()) + ".db";
        std::remove(path.c_str());
        page_id_t root;
        {
            Pager pager;
            ASSERT_TRUE(pager.Open(path));
            BufferPoolManager bpm(4, &pager);
            auto tree = BTree::Create(&bpm);
            root = tree->GetRootPageId();
            for (int k = 0; k < 3000; ++k)
                tree->Insert(Value(k * 2), RID(k + 1, 0));
        }
        {
            Pager pager;
            ASSERT_TRUE(pager.Open(path));
            BufferPoolManager bpm(4, &pager);
            BTree tree(&bpm, root);
            ASSERT_EQ(tree.Search(Value(4242)).size(), 1u);
            EXPECT_EQ(tree.Search(Value(4242))[0], RID(2122, 0));
            EXPECT_TRUE(tree.Search(Value(4243)).empty());
            EXPECT_EQ(tree.RangeScan(Value(0), true, Value(99), true).size(), 50u);
        }
        std::remove(path.c_str());
    }

} // namespace sql
