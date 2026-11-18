#pragma once

#include "net/protocol.h"
#include <cstdint>
#include <string>

namespace sql::net
{

    // An owned socket descriptor, closed on destruction
    class Socket
    {
    public:
        Socket() = default;
        explicit Socket(int fd) : fd_(fd) {}
        ~Socket() { Close(); }
        Socket(Socket &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
        Socket &operator=(Socket &&other) noexcept;
        Socket(const Socket &) = delete;
        Socket &operator=(const Socket &) = delete;

        int Fd() const { return fd_; }
        bool IsOpen() const { return fd_ >= 0; }
        void Close();
        // Wakes any thread blocked reading or writing it (safe from another
        // thread, unlike Close)
        void Shutdown();

    private:
        int fd_ = -1;
    };

    // Connects over TCP, giving up on an address after timeout_ms; throws
    // std::runtime_error on failure. The connection detects a vanished peer
    // (see DetectDeadPeer).
    Socket Connect(const std::string &host, uint16_t port, int timeout_ms = 2000);

    // A peer whose machine or container disappears sends no reset: without
    // this, a read waits for minutes. With it, the kernel probes an idle
    // connection after 2 s and gives up after about 5 s without an answer.
    // A slow but live peer is never cut off (its kernel answers).
    //
    // bound_unacknowledged also fails a send the peer has not acknowledged
    // in 10 s (a request sent just as it vanished). That includes a peer
    // whose application stops reading, so it is only for connections whose
    // reader is our own code (Connect), not for clients a server accepts: a
    // client suspended with Ctrl-Z keeps its session.
    void DetectDeadPeer(int fd, bool bound_unacknowledged);

    // Listens on host:port (port 0 picks a free one, reported in *bound_port);
    // throws std::runtime_error on failure
    Socket Listen(const std::string &host, uint16_t port, uint16_t *bound_port);

    // Sends one frame; throws std::runtime_error if the peer is gone
    void WriteFrame(const Socket &socket, const Frame &frame);

    // Receives one frame. Returns false if the peer closed the connection
    // cleanly between frames; throws std::runtime_error on a broken or
    // malformed stream (cut mid-frame, empty or oversized frame)
    bool ReadFrame(const Socket &socket, Frame *frame);

} // namespace sql::net
