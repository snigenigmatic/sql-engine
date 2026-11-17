#include "server/server.h"
#include "cli/local_connection.h"
#include "session/session.h"
#include <cerrno>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>

namespace sql
{

    void Server::Start(const std::string &host, uint16_t port)
    {
        listener_ = net::Listen(host, port, &port_);
        accept_thread_ = std::thread([this] { AcceptLoop(); });
    }

    void Server::AcceptLoop()
    {
        while (!stopping_)
        {
            const int fd = ::accept(listener_.Fd(), nullptr, nullptr);
            if (fd < 0)
            {
                if (stopping_)
                    break;
                if (errno != EINTR)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // e.g. out of descriptors
                continue;
            }
            Reap();
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (stopping_)
            {
                ::close(fd);
                break;
            }
            auto client = std::make_unique<Client>();
            client->socket = net::Socket(fd);
            Client *raw = client.get();
            client->thread = std::thread([this, raw] { Serve(raw); });
            clients_.push_back(std::move(client));
        }
    }

    void Server::Serve(Client *client)
    {
        // Declared before the session, so the database stays locked while
        // the session closes and rolls back an open transaction
        std::unique_lock<std::mutex> hold(db_mutex_, std::defer_lock);
        {
            Session session(db_);
            LocalConnection connection(db_, &session);
            try
            {
                net::Frame request;
                while (net::ReadFrame(client->socket, &request))
                {
                    if (!hold.owns_lock())
                        hold.lock();
                    net::Frame response;
                    try
                    {
                        if (auto execute = net::DecodeExecute(request))
                        {
                            net::ResultsResponse results;
                            switch (execute->mode)
                            {
                            case net::ExecuteMode::STATEMENT:
                                results.results.push_back(connection.Execute(execute->sql));
                                break;
                            case net::ExecuteMode::SCRIPT:
                                results.results = connection.ExecuteScript(execute->sql, false);
                                break;
                            case net::ExecuteMode::SCRIPT_STOP_ON_ERROR:
                                results.results = connection.ExecuteScript(execute->sql, true);
                                break;
                            }
                            results.in_transaction = connection.InTransaction();
                            response = net::Encode(results);
                        }
                        else if (auto command = net::DecodeCommand(request))
                            response = net::Encode(net::TextResponse{
                                connection.InTransaction(), connection.Command(command->name, command->argument)});
                        else
                            response = net::EncodeError("malformed request");
                    }
                    catch (const std::exception &e)
                    {
                        response = net::EncodeError(e.what());
                    }
                    // Between transactions the database is free for others
                    if (!session.InTransaction())
                        hold.unlock();
                    net::WriteFrame(client->socket, response);
                }
            }
            catch (const std::exception &)
            {
                // A broken connection ends the session like a disconnect
            }
            if (session.InTransaction() && !hold.owns_lock())
                hold.lock(); // cannot happen: a transaction keeps the lock
        }
        client->done = true;
    }

    void Server::Reap()
    {
        std::list<std::unique_ptr<Client>> finished;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (auto it = clients_.begin(); it != clients_.end();)
            {
                if ((*it)->done)
                {
                    finished.push_back(std::move(*it));
                    it = clients_.erase(it);
                }
                else
                    ++it;
            }
        }
        for (auto &client : finished)
            client->thread.join();
    }

    void Server::Stop()
    {
        if (stopping_.exchange(true))
            return;
        listener_.Shutdown(); // wakes accept()
        if (accept_thread_.joinable())
            accept_thread_.join();
        listener_.Close();
        std::list<std::unique_ptr<Client>> clients;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients.swap(clients_);
        }
        for (auto &client : clients)
            client->socket.Shutdown(); // wakes its thread's read
        for (auto &client : clients)
            client->thread.join();
    }

} // namespace sql
