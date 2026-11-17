// sqlnode: serves a database file to clients over TCP
// (sqlengine --connect host:port)

#include "catalog/database.h"
#include "server/server.h"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    void Usage(const char *program)
    {
        std::cout << "Usage: " << program << " [--database FILE] [--host HOST] [--port PORT]\n"
                  << "  --database FILE  database to serve (default: sqlengine.db)\n"
                  << "  --host HOST      address to listen on (default: 0.0.0.0, all)\n"
                  << "  --port PORT      TCP port (default: 7480; 0 picks a free one)\n"
                  << "Stops cleanly on SIGINT or SIGTERM.\n";
    }
} // namespace

int main(int argc, char **argv)
{
    std::string path = "sqlengine.db", host = "0.0.0.0";
    long port = 7480;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "-h" || arg == "--help")
        {
            Usage(argv[0]);
            return 0;
        }
        else if (arg == "--database" && has_value)
            path = argv[++i];
        else if (arg == "--host" && has_value)
            host = argv[++i];
        else if (arg == "--port" && has_value)
        {
            char *end = nullptr;
            port = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || port < 0 || port > 65535)
            {
                std::cerr << "Error: bad port " << argv[i] << "\n";
                return 1;
            }
        }
        else
        {
            Usage(argv[0]);
            return 1;
        }
    }

    // Block the stop signals in every thread; the main thread waits for them
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    std::string error;
    auto db = sql::Database::Open(path, &error);
    if (!db)
    {
        std::cerr << "Error: " << error << "\n";
        return 1;
    }
    sql::Server server(db.get());
    try
    {
        server.Start(host, static_cast<uint16_t>(port));
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    std::cout << "sqlnode: serving " << path << " on " << host << ":" << server.Port() << std::endl;

    int signal = 0;
    sigwait(&stop_signals, &signal);
    std::cout << "sqlnode: stopping (" << (signal == SIGINT ? "SIGINT" : "SIGTERM") << ")" << std::endl;
    server.Stop(); // disconnects clients, rolling back open transactions
    db.reset();    // checkpoints
    return 0;
}
