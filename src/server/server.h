#pragma once

#include "catalog/database.h"
#include "net/socket.h"
#include "session/connection.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace sql
{

    // Serves Connections over the network (see net/protocol.h): each
    // client gets its own Connection, made by a factory, on its own thread.
    //
    // Serving one database (the Database constructor): each client gets its
    // own Session. The engine allows one writer at a time, so requests take
    // turns on the database, and a client with an open transaction keeps it
    // to itself until COMMIT, ROLLBACK or disconnect (which rolls the
    // transaction back): clients are serializable, and one idle in a
    // transaction blocks the others.
    class Server
    {
    public:
        using ConnectionFactory = std::function<std::unique_ptr<Connection>()>;

        explicit Server(Database *db);
        // `exclusive`: requests take turns, and a client inside a
        // transaction holds the turn until it ends (for a single database)
        Server(ConnectionFactory factory, bool exclusive);
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

        ConnectionFactory factory_;
        bool exclusive_;
        net::Socket listener_;
        uint16_t port_ = 0;
        std::thread accept_thread_;
        std::atomic<bool> stopping_{false};

        std::mutex clients_mutex_;
        std::list<std::unique_ptr<Client>> clients_;

        std::mutex turn_mutex_; // with `exclusive`: held by the client whose turn it is
    };

} // namespace sql
