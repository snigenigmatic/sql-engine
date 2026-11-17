#include <gtest/gtest.h>
#include "cluster/raft/memory_state_manager.h"
#include "cluster/raft/raft_logger.h"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// D0: NuRaft runs in this build. Real Raft servers on localhost ports
// replicate a log, survive the loss of their leader, and several Raft
// groups (as on a storage pod holding many shards) share one process.

namespace sql::cluster
{

    namespace
    {
        using namespace std::chrono_literals;

        // Appends each committed number; no snapshots yet (that is D3)
        class ListStateMachine : public nuraft::state_machine
        {
        public:
            nuraft::ptr<nuraft::buffer> commit(const nuraft::ulong log_idx, nuraft::buffer &data) override
            {
                data.pos(0);
                const nuraft::ulong value = data.get_ulong();
                std::lock_guard<std::mutex> lock(mutex_);
                values_.push_back(value);
                last_commit_ = log_idx;
                return nullptr;
            }
            bool apply_snapshot(nuraft::snapshot &) override { return false; }
            nuraft::ptr<nuraft::snapshot> last_snapshot() override { return nullptr; }
            nuraft::ulong last_commit_index() override { return last_commit_; }
            void create_snapshot(nuraft::snapshot &, nuraft::async_result<bool>::handler_type &when_done) override
            {
                nuraft::ptr<std::exception> no_error;
                bool created = false;
                when_done(created, no_error);
            }

            std::vector<nuraft::ulong> Values()
            {
                std::lock_guard<std::mutex> lock(mutex_);
                return values_;
            }

        private:
            std::mutex mutex_;
            std::vector<nuraft::ulong> values_;
            std::atomic<nuraft::ulong> last_commit_{0};
        };

        // A port nothing listens on right now, and not handed out before:
        // the kernel may offer a just-closed port again
        int FreePort()
        {
            static std::set<int> handed_out;
            for (;;)
            {
                const int fd = socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = 0;
                bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
                socklen_t length = sizeof(addr);
                getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &length);
                close(fd);
                const int port = ntohs(addr.sin_port);
                if (handed_out.insert(port).second)
                    return port;
            }
        }

        bool WaitFor(const std::function<bool()> &condition, std::chrono::milliseconds limit = 10s)
        {
            const auto deadline = std::chrono::steady_clock::now() + limit;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (condition())
                    return true;
                std::this_thread::sleep_for(20ms);
            }
            return condition();
        }

        struct Server
        {
            int id = 0;
            int port = 0;
            nuraft::ptr<ListStateMachine> machine;
            nuraft::ptr<MemoryStateManager> state;
            std::unique_ptr<nuraft::raft_launcher> launcher;
            nuraft::ptr<nuraft::raft_server> raft;

            bool Running() const { return raft != nullptr; }
            void Stop()
            {
                if (!raft)
                    return;
                launcher->shutdown(5);
                raft.reset();
            }
        };

        // One Raft group of servers on localhost. Logging is fatal-only:
        // NuRaft reports every socket closed at shutdown as an error
        class Group
        {
        public:
            explicit Group(int size)
            {
                for (int i = 1; i <= size; ++i)
                {
                    auto server = std::make_unique<Server>();
                    server->id = i;
                    server->port = FreePort();
                    server->machine = nuraft::cs_new<ListStateMachine>();
                    server->state =
                        nuraft::cs_new<MemoryStateManager>(i, "127.0.0.1:" + std::to_string(server->port));
                    server->launcher = std::make_unique<nuraft::raft_launcher>();
                    nuraft::raft_params params;
                    params.heart_beat_interval_ = 50;
                    params.election_timeout_lower_bound_ = 200;
                    params.election_timeout_upper_bound_ = 400;
                    params.snapshot_distance_ = 0; // no snapshots
                    params.client_req_timeout_ = 3000;
                    params.return_method_ = nuraft::raft_params::blocking;
                    nuraft::asio_service::options asio;
                    asio.thread_pool_size_ = 2;
                    server->raft = server->launcher->init(server->machine, server->state,
                                                          nuraft::cs_new<RaftLogger>(1), server->port, asio, params);
                    servers_.push_back(std::move(server));
                }
            }

            ~Group()
            {
                for (auto &server : servers_)
                    server->Stop();
            }

            Server &operator[](size_t i) { return *servers_[i]; }
            size_t Size() const { return servers_.size(); }

            // Server 1 starts as a one-member cluster; the others join it
            ::testing::AssertionResult Form()
            {
                Server &first = *servers_[0];
                if (!first.raft)
                    return ::testing::AssertionFailure() << "server 1 did not start";
                if (!WaitFor([&] { return first.raft->is_leader(); }))
                    return ::testing::AssertionFailure() << "server 1 never became leader";
                for (size_t i = 1; i < servers_.size(); ++i)
                {
                    Server &joiner = *servers_[i];
                    // One membership change at a time: the leader refuses a
                    // new one (CONFIG_CHANGING) until the last is committed,
                    // including its own initial configuration
                    nuraft::cmd_result_code code = nuraft::cmd_result_code::OK;
                    if (!WaitFor(
                            [&]
                            {
                                auto added = first.raft->add_srv(*joiner.state->GetServerConfig());
                                code = added->get_result_code();
                                return added->get_accepted();
                            }))
                        return ::testing::AssertionFailure() << "add_srv " << joiner.id << " refused: " << code;
                    if (!WaitFor([&] { return first.raft->get_config()->get_server(joiner.id) != nullptr; }))
                        return ::testing::AssertionFailure() << "server " << joiner.id << " never joined";
                }
                return ::testing::AssertionSuccess();
            }

            Server *Leader()
            {
                for (auto &server : servers_)
                    if (server->Running() && server->raft->is_leader())
                        return server.get();
                return nullptr;
            }

            // Appends through the current leader; false if not accepted
            bool Append(nuraft::ulong value)
            {
                Server *leader = nullptr;
                if (!WaitFor([&] { return (leader = Leader()) != nullptr; }))
                    return false;
                nuraft::ptr<nuraft::buffer> entry = nuraft::buffer::alloc(sizeof(nuraft::ulong));
                entry->put(value);
                entry->pos(0);
                auto result = leader->raft->append_entries({entry});
                return result->get_accepted() && result->get_result_code() == nuraft::cmd_result_code::OK;
            }

            // Every running server applied exactly `expected`, in order
            bool AllApplied(const std::vector<nuraft::ulong> &expected)
            {
                return WaitFor(
                    [&]
                    {
                        for (auto &server : servers_)
                            if (server->Running() && server->machine->Values() != expected)
                                return false;
                        return true;
                    });
            }

        private:
            std::vector<std::unique_ptr<Server>> servers_;
        };
    } // namespace

    TEST(NuRaftTest, ThreeReplicasApplyTheSameLog)
    {
        Group group(3);
        ASSERT_TRUE(group.Form());
        std::vector<nuraft::ulong> expected;
        for (nuraft::ulong v = 1; v <= 50; ++v)
        {
            ASSERT_TRUE(group.Append(v * 7));
            expected.push_back(v * 7);
        }
        EXPECT_TRUE(group.AllApplied(expected));
        for (size_t i = 0; i < group.Size(); ++i)
            EXPECT_EQ(group[i].machine->Values(), expected) << "server " << group[i].id;
        // The membership itself is replicated
        for (size_t i = 0; i < group.Size(); ++i)
            EXPECT_EQ(group[i].state->load_config()->get_servers().size(), 3u) << "server " << group[i].id;
    }

    TEST(NuRaftTest, SurvivesTheLossOfItsLeader)
    {
        Group group(3);
        ASSERT_TRUE(group.Form());
        std::vector<nuraft::ulong> expected;
        for (nuraft::ulong v = 1; v <= 10; ++v)
        {
            ASSERT_TRUE(group.Append(v));
            expected.push_back(v);
        }
        ASSERT_TRUE(group.AllApplied(expected));

        Server *old_leader = group.Leader();
        ASSERT_NE(old_leader, nullptr);
        const int old_id = old_leader->id;
        old_leader->Stop();

        // The two survivors are still a quorum: one of them takes over
        ASSERT_TRUE(WaitFor([&] { return group.Leader() != nullptr; }));
        EXPECT_NE(group.Leader()->id, old_id);
        for (nuraft::ulong v = 11; v <= 20; ++v)
        {
            ASSERT_TRUE(group.Append(v));
            expected.push_back(v);
        }
        EXPECT_TRUE(group.AllApplied(expected));
    }

    TEST(NuRaftTest, LosingAQuorumStopsWrites)
    {
        Group group(3);
        ASSERT_TRUE(group.Form());
        ASSERT_TRUE(group.Append(1));
        ASSERT_TRUE(group.AllApplied({1}));
        // Stop both followers: the leader alone cannot commit anything
        for (size_t i = 0; i < group.Size(); ++i)
            if (!group[i].raft->is_leader())
                group[i].Stop();
        Server *leader = group.Leader();
        ASSERT_NE(leader, nullptr);
        nuraft::ptr<nuraft::buffer> entry = nuraft::buffer::alloc(sizeof(nuraft::ulong));
        entry->put(static_cast<nuraft::ulong>(2));
        entry->pos(0);
        auto result = leader->raft->append_entries({entry});
        EXPECT_NE(result->get_result_code(), nuraft::cmd_result_code::OK);
        EXPECT_EQ(leader->machine->Values(), (std::vector<nuraft::ulong>{1}));
    }

    TEST(NuRaftTest, SeveralGroupsShareOneProcess)
    {
        // As on a storage pod: one Raft server per shard replica, each with
        // its own port, all in one process
        Group a(3), b(3);
        ASSERT_TRUE(a.Form());
        ASSERT_TRUE(b.Form());
        std::vector<nuraft::ulong> in_a, in_b;
        for (nuraft::ulong v = 1; v <= 20; ++v)
        {
            ASSERT_TRUE(a.Append(v));
            in_a.push_back(v);
            ASSERT_TRUE(b.Append(1000 + v));
            in_b.push_back(1000 + v);
        }
        EXPECT_TRUE(a.AllApplied(in_a));
        EXPECT_TRUE(b.AllApplied(in_b));
    }

} // namespace sql::cluster
