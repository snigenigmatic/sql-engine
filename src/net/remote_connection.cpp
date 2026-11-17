#include "net/remote_connection.h"
#include <stdexcept>

namespace sql::net
{

    RemoteConnection::RemoteConnection(const std::string &host, uint16_t port)
        : socket_(Connect(host, port)), address_(host + ":" + std::to_string(port))
    {
    }

    Frame RemoteConnection::RoundTrip(const Frame &request)
    {
        if (!socket_.IsOpen())
            throw std::runtime_error("not connected to " + address_);
        Frame response;
        try
        {
            WriteFrame(socket_, request);
            if (!ReadFrame(socket_, &response))
                throw std::runtime_error("server closed the connection");
        }
        catch (const std::exception &e)
        {
            // Whatever the server was doing, its session is gone with the
            // connection (an open transaction was rolled back)
            socket_.Close();
            in_transaction_ = false;
            throw std::runtime_error("lost connection to " + address_ + ": " + e.what());
        }
        if (auto message = DecodeError(response))
            throw std::runtime_error(*message);
        return response;
    }

    std::vector<ExecutionResult> RemoteConnection::Run(ExecuteMode mode, const std::string &sql)
    {
        auto response = DecodeResults(RoundTrip(Encode(ExecuteRequest{mode, sql})));
        if (!response)
            throw std::runtime_error("malformed response from " + address_);
        in_transaction_ = response->in_transaction;
        return std::move(response->results);
    }

    ExecutionResult RemoteConnection::Execute(const std::string &sql)
    {
        auto results = Run(ExecuteMode::STATEMENT, sql);
        if (results.size() != 1)
            throw std::runtime_error("malformed response from " + address_);
        return std::move(results.front());
    }

    std::vector<ExecutionResult> RemoteConnection::ExecuteScript(const std::string &sql, bool stop_on_error)
    {
        return Run(stop_on_error ? ExecuteMode::SCRIPT_STOP_ON_ERROR : ExecuteMode::SCRIPT, sql);
    }

    std::string RemoteConnection::Command(const std::string &name, const std::string &argument)
    {
        auto response = DecodeText(RoundTrip(Encode(CommandRequest{name, argument})));
        if (!response)
            throw std::runtime_error("malformed response from " + address_);
        in_transaction_ = response->in_transaction;
        // Where the database lives, as seen from here
        if (name == "describe")
            return "Server: " + address_ + "\n" + response->text;
        return response->text;
    }

} // namespace sql::net
