#pragma once

#include "catalog/database.h"
#include "net/socket.h"
#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace sql
{

    // Serves one database over the network (see net/protocol.h). Each
    // client gets its own Session on its own thread. The engine allows one
    // writer at a time, so requests take turns on the database, and a
    // client with an open transaction keeps it to itself until COMMIT,
    // ROLLBACK or disconnect (which rolls the transaction back): clients
    // are serializable, and one idle in a transaction blocks the others.
    class Server
    {
    public:
        explicit Server(Database *db) : db_(db) {}
        ~Server() { Stop(); }

        Server(const Server &) = delete;
        Server &operator=(const Server &) = delete;

        // Listen on host:port (port 0: any free port) and start accepting.
        // Throws std::runtime_error if it cannot listen.
        void Start(const std::string &host, uint16_t port);

        uint16_t Port() const { return port_; }

        // Stop accepting, disconnect every client (rolling back their open
        // transactions) and wait for their threads. Idempotent.
        void Stop();

    private:
        struct Client
        {
            net::Socket socket;
            std::thread thread;
            std::atomic<bool> done{false};
        };

        void AcceptLoop();
        void Serve(Client *client);
        // Joins the threads of clients that disconnected
        void Reap();

        Database *db_;
        net::Socket listener_;
        uint16_t port_ = 0;
        std::thread accept_thread_;
        std::atomic<bool> stopping_{false};

        std::mutex clients_mutex_;
        std::list<std::unique_ptr<Client>> clients_;

        std::mutex db_mutex_; // held by the client using the database
    };

} // namespace sql
