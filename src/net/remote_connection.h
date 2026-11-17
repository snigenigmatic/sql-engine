#pragma once

#include "net/socket.h"
#include "session/connection.h"
#include <cstdint>
#include <string>

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

        ExecutionResult Execute(const std::string &sql) override;
        std::vector<ExecutionResult> ExecuteScript(const std::string &sql, bool stop_on_error = false) override;
        // As of the last response: no round trip
        bool InTransaction() override { return in_transaction_; }
        std::string Command(const std::string &name, const std::string &argument) override;
        ExecutionResult Call(const std::string &method, const std::vector<std::string> &arguments) override;

    private:
        std::vector<ExecutionResult> Run(ExecuteMode mode, const std::string &sql);
        Frame RoundTrip(const Frame &request);

        Socket socket_;
        std::string address_; // host:port, for messages
        bool in_transaction_ = false;
    };

} // namespace sql::net
