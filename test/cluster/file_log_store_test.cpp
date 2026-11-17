#include <gtest/gtest.h>
#include "cluster/shard/file_log_store.h"
#include "cluster/shard/file_state_manager.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

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

        std::vector<std::pair<nuraft::ulong, nuraft::ulong>> Contents(FileLogStore &store)
        {
            std::vector<std::pair<nuraft::ulong, nuraft::ulong>> out;
            const auto entries = store.log_entries(store.start_index(), store.next_slot());
            for (const auto &entry : *entries)
                out.emplace_back(entry->get_term(), ValueOf(entry));
            return out;
        }

        void Append(FileLogStore &store, nuraft::ulong term, nuraft::ulong value)
        {
            auto entry = Entry(term, value);
            store.append(entry);
            store.end_of_append_batch(0, 1);
        }

        struct TempDir
        {
            std::string path;
            TempDir()
            {
                static int counter = 0;
                path = (fs::temp_directory_path() /
                        ("raftlog_" + std::to_string(getpid()) + "_" + std::to_string(counter++)))
                           .string();
                fs::remove_all(path);
                fs::create_directories(path);
            }
            ~TempDir() { fs::remove_all(path); }
        };

        using Pairs = std::vector<std::pair<nuraft::ulong, nuraft::ulong>>;
    } // namespace

    TEST(FileLogStoreTest, SurvivesReopening)
    {
        TempDir dir;
        {
            FileLogStore store(dir.path);
            EXPECT_EQ(store.start_index(), 1u);
            EXPECT_EQ(store.next_slot(), 1u);
            for (nuraft::ulong i = 1; i <= 5; ++i)
                Append(store, i < 3 ? 1 : 2, i * 10);
        }
        FileLogStore store(dir.path);
        EXPECT_EQ(store.DroppedOnOpen(), 0u);
        EXPECT_EQ(store.next_slot(), 6u);
        EXPECT_EQ(Contents(store), (Pairs{{1, 10}, {1, 20}, {2, 30}, {2, 40}, {2, 50}}));
        EXPECT_EQ(store.term_at(4), 2u);
        Append(store, 3, 60);
        EXPECT_EQ(store.next_slot(), 7u);
    }

    TEST(FileLogStoreTest, OverwritesAndCompactionArePersisted)
    {
        TempDir dir;
        {
            FileLogStore store(dir.path);
            for (nuraft::ulong i = 1; i <= 6; ++i)
                Append(store, 1, i);
            auto replacement = Entry(2, 400);
            store.write_at(4, replacement); // drops 5 and 6
            EXPECT_TRUE(store.compact(2));
            EXPECT_EQ(store.start_index(), 3u);
        }
        {
            FileLogStore store(dir.path);
            EXPECT_EQ(store.start_index(), 3u);
            EXPECT_EQ(Contents(store), (Pairs{{1, 3}, {2, 400}}));
            Append(store, 2, 500);
            // Past the end: the log restarts there (after a snapshot install)
            auto far = Entry(3, 900);
            store.write_at(20, far);
            EXPECT_EQ(store.start_index(), 20u);
        }
        FileLogStore store(dir.path);
        EXPECT_EQ(store.start_index(), 20u);
        EXPECT_EQ(Contents(store), (Pairs{{3, 900}}));
    }

    TEST(FileLogStoreTest, PacksRoundTripIntoAFile)
    {
        TempDir a, b;
        FileLogStore source(a.path);
        for (nuraft::ulong i = 1; i <= 5; ++i)
            Append(source, 1, i * 7);
        auto pack = source.pack(2, 3);
        {
            FileLogStore target(b.path);
            Append(target, 1, 7);
            Append(target, 1, 999); // replaced by the pack
            target.apply_pack(2, *pack);
        }
        FileLogStore target(b.path);
        EXPECT_EQ(Contents(target), (Pairs{{1, 7}, {1, 14}, {1, 21}, {1, 28}}));
    }

    TEST(FileLogStoreTest, ATornTailIsCutOff)
    {
        TempDir dir;
        {
            FileLogStore store(dir.path);
            for (nuraft::ulong i = 1; i <= 3; ++i)
                Append(store, 1, i);
        }
        const std::string log = dir.path + "/log";
        const auto size = fs::file_size(log);
        fs::resize_file(log, size - 3); // a crash in the middle of the last record
        {
            FileLogStore store(dir.path);
            EXPECT_EQ(store.DroppedOnOpen(), 1u);
            EXPECT_EQ(Contents(store), (Pairs{{1, 1}, {1, 2}}));
            Append(store, 2, 30); // appends after the good records
        }
        FileLogStore store(dir.path);
        EXPECT_EQ(store.DroppedOnOpen(), 0u);
        EXPECT_EQ(Contents(store), (Pairs{{1, 1}, {1, 2}, {2, 30}}));
    }

    TEST(FileLogStoreTest, ACorruptRecordEndsTheLog)
    {
        TempDir dir;
        {
            FileLogStore store(dir.path);
            for (nuraft::ulong i = 1; i <= 4; ++i)
                Append(store, 1, i);
        }
        // Flip a byte inside the second record's body
        const std::string log = dir.path + "/log";
        std::fstream file(log, std::ios::in | std::ios::out | std::ios::binary);
        const auto record = (fs::file_size(log) - 16) / 4; // all records are the same size
        file.seekp(static_cast<std::streamoff>(16 + record + record - 1));
        file.put('\x5a');
        file.close();
        FileLogStore store(dir.path);
        EXPECT_EQ(store.DroppedOnOpen(), 1u);
        EXPECT_EQ(Contents(store), (Pairs{{1, 1}}));
    }

    TEST(FileLogStoreTest, RefusesAFileThatIsNotALog)
    {
        TempDir dir;
        std::ofstream(dir.path + "/log") << "definitely not a raft log";
        EXPECT_THROW(FileLogStore store(dir.path), std::runtime_error);
    }

    TEST(FileStateManagerTest, StateAndMembershipSurviveRestarts)
    {
        TempDir dir;
        auto initial = nuraft::cs_new<nuraft::cluster_config>();
        initial->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, "a:1"));
        initial->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(2, "b:2"));
        {
            FileStateManager manager(1, dir.path, initial);
            EXPECT_EQ(manager.read_state(), nullptr);
            EXPECT_EQ(manager.load_config()->get_servers().size(), 2u);
            nuraft::srv_state state(7, 2, false);
            manager.save_state(state);
            auto grown = nuraft::cs_new<nuraft::cluster_config>(5, 4);
            grown->get_servers() = initial->get_servers();
            grown->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(3, "c:3"));
            manager.save_config(*grown);
        }
        FileStateManager manager(1, dir.path, initial); // the saved config wins
        auto state = manager.read_state();
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->get_term(), 7u);
        EXPECT_EQ(state->get_voted_for(), 2);
        auto config = manager.load_config();
        EXPECT_EQ(config->get_servers().size(), 3u);
        EXPECT_EQ(config->get_log_idx(), 5u);
    }

} // namespace sql::cluster
