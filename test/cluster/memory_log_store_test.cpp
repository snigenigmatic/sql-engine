#include <gtest/gtest.h>
#include "cluster/raft/memory_log_store.h"
#include <vector>

namespace sql::cluster
{

    namespace
    {
        nuraft::ptr<nuraft::log_entry> Entry(nuraft::ulong term, nuraft::ulong value)
        {
            nuraft::ptr<nuraft::buffer> data = nuraft::buffer::alloc(sizeof(nuraft::ulong));
            data->put(value);
            data->pos(0);
            return nuraft::cs_new<nuraft::log_entry>(term, data);
        }

        nuraft::ulong ValueOf(const nuraft::ptr<nuraft::log_entry> &entry)
        {
            entry->get_buf().pos(0);
            return entry->get_buf().get_ulong();
        }

        // (term, value) of every stored entry
        std::vector<std::pair<nuraft::ulong, nuraft::ulong>> Contents(MemoryLogStore &store)
        {
            std::vector<std::pair<nuraft::ulong, nuraft::ulong>> out;
            const auto entries = store.log_entries(store.start_index(), store.next_slot());
            for (const auto &entry : *entries)
                out.emplace_back(entry->get_term(), ValueOf(entry));
            return out;
        }
    } // namespace

    TEST(MemoryLogStoreTest, AppendsFromIndexOne)
    {
        MemoryLogStore store;
        EXPECT_EQ(store.start_index(), 1u);
        EXPECT_EQ(store.next_slot(), 1u);
        EXPECT_EQ(store.last_entry()->get_term(), 0u); // the dummy
        for (nuraft::ulong i = 1; i <= 3; ++i)
        {
            auto entry = Entry(1, i * 10);
            EXPECT_EQ(store.append(entry), i);
        }
        EXPECT_EQ(store.next_slot(), 4u);
        EXPECT_EQ(ValueOf(store.last_entry()), 30u);
        EXPECT_EQ(ValueOf(store.entry_at(2)), 20u);
        EXPECT_EQ(store.term_at(3), 1u);
        EXPECT_EQ(store.term_at(9), 0u); // out of range: the dummy's term
    }

    TEST(MemoryLogStoreTest, EntriesAreCopies)
    {
        MemoryLogStore store;
        auto entry = Entry(1, 5);
        store.append(entry);
        // Changing what was appended, or what was read, leaves the log alone
        entry->get_buf().pos(0);
        entry->get_buf().put(static_cast<nuraft::ulong>(99));
        auto read = store.entry_at(1);
        read->get_buf().pos(0);
        read->get_buf().put(static_cast<nuraft::ulong>(77));
        EXPECT_EQ(ValueOf(store.entry_at(1)), 5u);
    }

    TEST(MemoryLogStoreTest, WriteAtReplacesTheTail)
    {
        MemoryLogStore store;
        for (nuraft::ulong i = 1; i <= 5; ++i)
        {
            auto entry = Entry(1, i);
            store.append(entry);
        }
        // A new leader overwrites entries 3.. with its own
        auto replacement = Entry(2, 300);
        store.write_at(3, replacement);
        EXPECT_EQ(store.next_slot(), 4u);
        EXPECT_EQ(Contents(store), (std::vector<std::pair<nuraft::ulong, nuraft::ulong>>{{1, 1}, {1, 2}, {2, 300}}));
        // Writing at the next slot is an append
        auto next = Entry(2, 400);
        store.write_at(4, next);
        EXPECT_EQ(store.next_slot(), 5u);
    }

    TEST(MemoryLogStoreTest, CompactDropsThePrefix)
    {
        MemoryLogStore store;
        for (nuraft::ulong i = 1; i <= 5; ++i)
        {
            auto entry = Entry(1, i);
            store.append(entry);
        }
        EXPECT_TRUE(store.compact(2));
        EXPECT_EQ(store.start_index(), 3u);
        EXPECT_EQ(store.next_slot(), 6u);
        EXPECT_EQ(ValueOf(store.entry_at(3)), 3u);
        EXPECT_EQ(store.term_at(2), 0u); // compacted away
        // Compacting past the end (after a snapshot) empties the log and
        // moves its start
        EXPECT_TRUE(store.compact(9));
        EXPECT_EQ(store.start_index(), 10u);
        EXPECT_EQ(store.next_slot(), 10u);
        auto entry = Entry(3, 10);
        EXPECT_EQ(store.append(entry), 10u);
    }

    TEST(MemoryLogStoreTest, PackRoundTrips)
    {
        MemoryLogStore source;
        for (nuraft::ulong i = 1; i <= 6; ++i)
        {
            auto entry = Entry(i <= 3 ? 1 : 2, i * 100);
            source.append(entry);
        }
        nuraft::ptr<nuraft::buffer> pack = source.pack(2, 4); // entries 2..5

        MemoryLogStore target;
        auto stale = Entry(1, 1);
        target.append(stale);
        auto conflicting = Entry(1, 999);
        target.append(conflicting); // index 2, to be replaced
        target.apply_pack(2, *pack);
        EXPECT_EQ(target.start_index(), 1u);
        EXPECT_EQ(target.next_slot(), 6u);
        EXPECT_EQ(Contents(target), (std::vector<std::pair<nuraft::ulong, nuraft::ulong>>{
                                        {1, 1}, {1, 200}, {1, 300}, {2, 400}, {2, 500}}));

        // Into an empty log, the pack sets where the log starts
        MemoryLogStore empty;
        empty.apply_pack(2, *pack);
        EXPECT_EQ(empty.start_index(), 2u);
        EXPECT_EQ(empty.next_slot(), 6u);
    }

} // namespace sql::cluster
