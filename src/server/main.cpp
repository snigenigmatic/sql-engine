// sqlnode: serves a database file to clients over TCP
// (sqlengine --connect host:port), or runs one process of a cluster

#include "catalog/database.h"
#include "server/server.h"
#ifdef SQLENGINE_DISTRIBUTED
#include "cluster/node/coordinator.h"
#include "cluster/node/storage_node.h"
#endif
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace
{
    void Usage(const char *program)
    {
        std::cout
            << "Usage: " << program << " [--role single] [--database FILE] [--host HOST] [--port PORT]\n"
#ifdef SQLENGINE_DISTRIBUTED
            << "       " << program
            << " --role coordinator --id N --coordinators SPEC --dir DIR [--shards S] [--rf R]\n"
            << "       " << program
            << " --role storage --name NAME --coordinators SPEC --dir DIR --raft-base PORT [--host HOST] [--port PORT]\n"
#endif
            << "  --database FILE  database to serve (single; default: sqlengine.db)\n"
            << "  --host HOST      address to listen on / to be reached at (default: 0.0.0.0, or 127.0.0.1)\n"
            << "  --port PORT      TCP port for clients (default: 7480; 0 picks a free one)\n"
#ifdef SQLENGINE_DISTRIBUTED
            << "  --coordinators   every coordinator, as id=host:raft_port:client_port,...\n"
            << "  --id N           this coordinator's id in --coordinators\n"
            << "  --dir DIR        where this process keeps its Raft logs and data\n"
            << "  --shards S       shards in a new cluster (default 16)\n"
            << "  --rf R           replicas of each shard (default 3)\n"
            << "  --name NAME      a storage node's stable name (e.g. node-0)\n"
            << "  --raft-base P    a storage node's replica of shard s listens on P + s\n"
#endif
            << "Stops cleanly on SIGINT or SIGTERM.\n";
    }

#ifdef SQLENGINE_DISTRIBUTED
    struct CoordinatorSpec
    {
        int id;
        std::string host;
        int raft_port, client_port;
    };

    // id=host:raft_port:client_port,...
    std::vector<CoordinatorSpec> ParseCoordinators(const std::string &text)
    {
        std::vector<CoordinatorSpec> specs;
        std::stringstream in(text);
        std::string item;
        while (std::getline(in, item, ','))
        {
            const size_t eq = item.find('='), c2 = item.rfind(':');
            const size_t c1 = item.rfind(':', c2 - 1);
            if (eq == std::string::npos || c1 == std::string::npos || c1 <= eq)
                throw std::invalid_argument("bad coordinator " + item + " (expected id=host:raft_port:client_port)");
            specs.push_back({std::stoi(item.substr(0, eq)), item.substr(eq + 1, c1 - eq - 1),
                             std::stoi(item.substr(c1 + 1, c2 - c1 - 1)), std::stoi(item.substr(c2 + 1))});
        }
        return specs;
    }
#endif

    int WaitForStopSignal(const sigset_t &signals)
    {
        int signal = 0;
        sigwait(&signals, &signal);
        std::cout << "sqlnode: stopping (" << (signal == SIGINT ? "SIGINT" : "SIGTERM") << ")" << std::endl;
        return signal;
    }
} // namespace

int main(int argc, char **argv)
{
    std::map<std::string, std::string> args = {{"--role", "single"},   {"--database", "sqlengine.db"},
                                               {"--host", ""},         {"--port", ""},
                                               {"--shards", "16"},     {"--rf", "3"}};
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            Usage(argv[0]);
            return 0;
        }
        if (arg.rfind("--", 0) != 0 || i + 1 >= argc)
        {
            Usage(argv[0]);
            return 1;
        }
        args[arg] = argv[++i];
    }

    // Block the stop signals in every thread; the main thread waits for them
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    try
    {
        const std::string role = args["--role"];
        if (role == "single")
        {
            const std::string host = args["--host"].empty() ? "0.0.0.0" : args["--host"];
            const long port = args["--port"].empty() ? 7480 : std::stol(args["--port"]);
            std::string error;
            auto db = sql::Database::Open(args["--database"], &error);
            if (!db)
            {
                std::cerr << "Error: " << error << "\n";
                return 1;
            }
            sql::Server server(db.get());
            server.Start(host, static_cast<uint16_t>(port));
            std::cout << "sqlnode: serving " << args["--database"] << " on " << host << ":" << server.Port()
                      << std::endl;
            WaitForStopSignal(stop_signals);
            server.Stop(); // disconnects clients, rolling back open transactions
            db.reset();    // checkpoints
            return 0;
        }
#ifdef SQLENGINE_DISTRIBUTED
        if (role == "coordinator")
        {
            const auto specs = ParseCoordinators(args.at("--coordinators"));
            sql::cluster::Coordinator::Options options;
            options.id = std::stoi(args.at("--id"));
            options.directory = args.at("--dir");
            options.shard_count = static_cast<uint32_t>(std::stoul(args["--shards"]));
            options.replication_factor = std::stoul(args["--rf"]);
            bool found = false;
            for (const auto &spec : specs)
            {
                options.peers.push_back({spec.id, spec.host + ":" + std::to_string(spec.raft_port)});
                options.client_addresses[spec.id] = spec.host + ":" + std::to_string(spec.client_port);
                if (spec.id == options.id)
                {
                    found = true;
                    options.raft_port = spec.raft_port;
                    options.client_port = static_cast<uint16_t>(spec.client_port);
                    options.host = args["--host"].empty() ? "0.0.0.0" : args["--host"];
                }
            }
            if (!found)
                throw std::invalid_argument("--id " + args["--id"] + " is not in --coordinators");
            sql::cluster::Coordinator coordinator(options);
            coordinator.Start();
            std::cout << "sqlnode: coordinator " << options.id << " serving clients on port " << coordinator.Port()
                      << std::endl;
            WaitForStopSignal(stop_signals);
            coordinator.Stop();
            return 0;
        }
        if (role == "storage")
        {
            sql::cluster::StorageNode::Options options;
            options.name = args.at("--name");
            options.host = args["--host"].empty() ? "127.0.0.1" : args["--host"];
            options.port = static_cast<uint16_t>(args["--port"].empty() ? 0 : std::stoi(args["--port"]));
            options.raft_port_base = std::stoi(args.at("--raft-base"));
            options.directory = args.at("--dir");
            for (const auto &spec : ParseCoordinators(args.at("--coordinators")))
                options.coordinators.push_back(spec.host + ":" + std::to_string(spec.client_port));
            sql::cluster::StorageNode node(options);
            node.Start();
            std::cout << "sqlnode: storage node " << options.name << " on port " << node.Port() << std::endl;
            WaitForStopSignal(stop_signals);
            node.Stop();
            return 0;
        }
#endif
        std::cerr << "Error: unknown role " << role << "\n";
        return 1;
    }
    catch (const std::out_of_range &)
    {
        std::cerr << "Error: missing a required option\n";
        Usage(argv[0]);
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
