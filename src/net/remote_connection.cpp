#include "net/remote_connection.h"
#include <stdexcept>

namespace sql::net
{

    RemoteConnection::RemoteConnection(const std::string &host, uint16_t port)
        : servers_{{host, port}}, socket_(Connect(host, port)), address_(host + ":" + std::to_string(port))
    {
    }

    RemoteConnection::RemoteConnection(const std::vector<std::pair<std::string, uint16_t>> &servers, bool reconnect)
        : servers_(servers), reconnect_(reconnect)
    {
        if (servers_.empty())
            throw std::invalid_argument("no server to connect to");
        ConnectAny();
    }

    void RemoteConnection::ConnectAny()
    {
        std::string errors;
        for (size_t i = 0; i < servers_.size(); ++i)
        {
            const size_t at = (current_ + i) % servers_.size();
            const auto &[host, port] = servers_[at];
            try
            {
                socket_ = Connect(host, port);
                current_ = at;
                address_ = host + ":" + std::to_string(port);
                return;
            }
            catch (const std::exception &e)
            {
                errors += std::string(errors.empty() ? "" : "; ") + e.what();
            }
        }
        throw std::runtime_error(errors);
    }

    std::string RemoteConnection::TakeNotice()
    {
        std::string notice;
        notice.swap(notice_);
        return notice;
    }

    Frame RemoteConnection::RoundTrip(const Frame &request)
    {
        if (!socket_.IsOpen())
        {
            if (!reconnect_)
                throw std::runtime_error("not connected to " + address_);
            const std::string lost = address_;
            ConnectAny(); // nothing was sent: safe to try another server
            notice_ = "(reconnected to " + address_ + ")";
            if (lost_transaction_)
            {
                lost_transaction_ = false;
                throw std::runtime_error("the connection to " + lost +
                                         " was lost with a transaction open, so the transaction was rolled back; "
                                         "this statement was not run (reconnected to " + address_ + ")");
            }
        }
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
            lost_transaction_ = in_transaction_;
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

    ExecutionResult RemoteConnection::Call(const std::string &method, const std::vector<std::string> &arguments)
    {
        auto response = DecodeResults(RoundTrip(Encode(CallRequest{method, arguments})));
        if (!response || response->results.size() != 1)
            throw std::runtime_error("malformed response from " + address_);
        in_transaction_ = response->in_transaction;
        return std::move(response->results.front());
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
