#pragma once

#include "net/socket.h"
#include "session/connection.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sql::net
{

    // A Connection to a database served by `sqlnode`. One request at a
    // time; a server-side transaction lasts until COMMIT/ROLLBACK or until
    // this connection closes (which rolls it back).
    class RemoteConnection : public Connection
    {
    public:
        // Throws std::runtime_error if the server cannot be reached
        RemoteConnection(const std::string &host, uint16_t port);

        // Any of several servers (e.g. a cluster's coordinators): the first
        // that answers. With reconnect, a lost connection is replaced at
        // the next request by one to the first server that answers, trying
        // them in turn. A transaction open when the connection was lost is
        // gone; the first request after that is refused (not sent) with an
        // error saying so, so no later statement of it runs on its own.
        // Throws std::runtime_error if none can be reached.
        RemoteConnection(const std::vector<std::pair<std::string, uint16_t>> &servers, bool reconnect);

        // "reconnected to ..." once after a reconnect, else ""
        std::string TakeNotice() override;

        ExecutionResult Execute(const std::string &sql) override;
        std::vector<ExecutionResult> ExecuteScript(const std::string &sql, bool stop_on_error = false) override;
        // As of the last response: no round trip
        bool InTransaction() override { return in_transaction_; }
        std::string Command(const std::string &name, const std::string &argument) override;
        ExecutionResult Call(const std::string &method, const std::vector<std::string> &arguments) override;

    private:
        std::vector<ExecutionResult> Run(ExecuteMode mode, const std::string &sql);
        Frame RoundTrip(const Frame &request);
        void ConnectAny(); // throws if no server answers

        std::vector<std::pair<std::string, uint16_t>> servers_;
        size_t current_ = 0;
        bool reconnect_ = false;
        Socket socket_;
        std::string address_; // host:port, for messages
        bool in_transaction_ = false;
        bool lost_transaction_ = false; // the connection went while one was open
        std::string notice_;
    };

} // namespace sql::net
