#include "net/event_loop.h"

#include <stdexcept>

namespace rtsp::net {

void EventLoop::clear() {
    interests_.clear();
}

void EventLoop::watch(SOCKET socket, bool read, bool write) {
    if (socket == INVALID_SOCKET || (!read && !write)) {
        throw std::invalid_argument("watch requires socket and read/write interest");
    }
    for (const auto& interest : interests_) {
        if (interest.socket == socket) {
            throw std::invalid_argument("socket already watched");
        }
    }
    if (interests_.size() >= capacity) {
        throw std::length_error("select socket limit reached");
    }
    interests_.push_back({socket, read, write});
}

std::vector<SocketEvent> EventLoop::wait(int timeout_ms) const {
    if (timeout_ms < 0 || interests_.empty()) {
        throw std::invalid_argument("wait requires interests and nonnegative timeout");
    }
    for (;;) {
        fd_set readers;
        fd_set writers;
        FD_ZERO(&readers);
        FD_ZERO(&writers);
        for (const auto& interest : interests_) {
            if (interest.readable) {
                FD_SET(interest.socket, &readers);
            }
            if (interest.writable) {
                FD_SET(interest.socket, &writers);
            }
        }
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        const int result = ::select(0, &readers, &writers, nullptr, &timeout);
        if (result == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEINTR) {
                continue;
            }
            detail::throw_socket_error("select", error);
        }
        std::vector<SocketEvent> events;
        for (const auto& interest : interests_) {
            const bool read = interest.readable && FD_ISSET(interest.socket, &readers);
            const bool write = interest.writable && FD_ISSET(interest.socket, &writers);
            if (read || write) {
                events.push_back({interest.socket, read, write});
            }
        }
        return events;
    }
}

}  // namespace rtsp::net
