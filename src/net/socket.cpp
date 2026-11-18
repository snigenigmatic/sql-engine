#include "net/socket.h"
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace sql::net
{

    namespace
    {
        std::runtime_error SystemError(const std::string &what)
        {
            return std::runtime_error(what + ": " + std::strerror(errno));
        }

        // Reads exactly size bytes. Returns how many arrived before the peer
        // closed (size on success); throws on a socket error.
        size_t ReadFully(int fd, char *data, size_t size)
        {
            size_t done = 0;
            while (done < size)
            {
                const ssize_t n = ::recv(fd, data + done, size - done, 0);
                if (n == 0)
                    return done;
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    throw SystemError("receive failed");
                }
                done += static_cast<size_t>(n);
            }
            return done;
        }
    } // namespace

    Socket &Socket::operator=(Socket &&other) noexcept
    {
        if (this != &other)
        {
            Close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    void Socket::Close()
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }

    void Socket::Shutdown()
    {
        if (fd_ >= 0)
            ::shutdown(fd_, SHUT_RDWR);
    }

    void DetectDeadPeer(int fd, bool bound_unacknowledged)
    {
        const int on = 1, idle = 2, interval = 1, count = 3;
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
        if (bound_unacknowledged)
        {
            const unsigned int user_timeout_ms = 10000;
            ::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof(user_timeout_ms));
        }
    }

    namespace
    {
        // connect(), giving up after timeout_ms; leaves the socket blocking
        bool ConnectWithin(int fd, const sockaddr *address, socklen_t length, int timeout_ms)
        {
            const int flags = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            int result = ::connect(fd, address, length);
            if (result != 0 && errno == EINPROGRESS)
            {
                pollfd waiting{fd, POLLOUT, 0};
                int ready;
                do
                    ready = ::poll(&waiting, 1, timeout_ms);
                while (ready < 0 && errno == EINTR);
                if (ready == 0)
                    errno = ETIMEDOUT;
                else if (ready > 0)
                {
                    int error = 0;
                    socklen_t size = sizeof(error);
                    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
                    errno = error;
                    result = error == 0 ? 0 : -1;
                }
            }
            const int saved = errno;
            ::fcntl(fd, F_SETFL, flags);
            errno = saved;
            return result == 0;
        }
    } // namespace

    Socket Connect(const std::string &host, uint16_t port, int timeout_ms)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *found = nullptr;
        const int status = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found);
        if (status != 0)
            throw std::runtime_error("cannot resolve " + host + ": " + ::gai_strerror(status));
        std::string last_error = "no address";
        for (addrinfo *a = found; a != nullptr; a = a->ai_next)
        {
            Socket socket(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
            if (!socket.IsOpen())
                continue;
            if (ConnectWithin(socket.Fd(), a->ai_addr, a->ai_addrlen, timeout_ms))
            {
                ::freeaddrinfo(found);
                const int on = 1; // requests are small: send them at once
                ::setsockopt(socket.Fd(), IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
                DetectDeadPeer(socket.Fd(), true);
                return socket;
            }
            last_error = std::strerror(errno);
        }
        ::freeaddrinfo(found);
        throw std::runtime_error("cannot connect to " + host + ":" + std::to_string(port) + ": " + last_error);
    }

    Socket Listen(const std::string &host, uint16_t port, uint16_t *bound_port)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo *found = nullptr;
        const int status = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), std::to_string(port).c_str(),
                                         &hints, &found);
        if (status != 0)
            throw std::runtime_error("cannot resolve " + host + ": " + ::gai_strerror(status));
        std::string last_error = "no address";
        for (addrinfo *a = found; a != nullptr; a = a->ai_next)
        {
            Socket socket(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
            if (!socket.IsOpen())
                continue;
            const int on = 1; // restart without waiting for old connections to time out
            ::setsockopt(socket.Fd(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
            if (::bind(socket.Fd(), a->ai_addr, a->ai_addrlen) == 0 && ::listen(socket.Fd(), 64) == 0)
            {
                ::freeaddrinfo(found);
                sockaddr_storage address{};
                socklen_t length = sizeof(address);
                ::getsockname(socket.Fd(), reinterpret_cast<sockaddr *>(&address), &length);
                if (bound_port)
                    *bound_port = ntohs(address.ss_family == AF_INET6
                                            ? reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port
                                            : reinterpret_cast<sockaddr_in *>(&address)->sin_port);
                return socket;
            }
            last_error = std::strerror(errno);
        }
        ::freeaddrinfo(found);
        throw std::runtime_error("cannot listen on " + host + ":" + std::to_string(port) + ": " + last_error);
    }

    void WriteFrame(const Socket &socket, const Frame &frame)
    {
        if (frame.payload.size() + 1 > MAX_FRAME_SIZE)
            throw std::runtime_error("message too large to send");
        const std::string bytes = EncodeFrame(frame);
        size_t done = 0;
        while (done < bytes.size())
        {
            // MSG_NOSIGNAL: a closed peer is an error here, not a SIGPIPE
            const ssize_t n = ::send(socket.Fd(), bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                throw SystemError("send failed");
            }
            done += static_cast<size_t>(n);
        }
    }

    bool ReadFrame(const Socket &socket, Frame *frame)
    {
        unsigned char header[5];
        const size_t got = ReadFully(socket.Fd(), reinterpret_cast<char *>(header), sizeof(header));
        if (got == 0)
            return false;
        if (got < sizeof(header))
            throw std::runtime_error("connection closed in the middle of a message");
        const uint32_t length = (uint32_t{header[0]} << 24) | (uint32_t{header[1]} << 16) |
                                (uint32_t{header[2]} << 8) | uint32_t{header[3]};
        if (length == 0 || length > MAX_FRAME_SIZE)
            throw std::runtime_error("bad message length " + std::to_string(length));
        frame->type = static_cast<MessageType>(header[4]);
        frame->payload.resize(length - 1);
        if (ReadFully(socket.Fd(), frame->payload.data(), frame->payload.size()) < frame->payload.size())
            throw std::runtime_error("connection closed in the middle of a message");
        return true;
    }

} // namespace sql::net
