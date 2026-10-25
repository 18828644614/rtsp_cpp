#pragma once

#include "net/tcp_socket.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rtsp {

class RtspServerConnection;

class RtspServer {
public:
    RtspServer(std::string address, std::uint16_t port, std::string input_path);
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    void run();
    void stop();

    std::uint16_t local_port() const;
    std::size_t connection_count() const;

private:
    void reap_finished_connections();
    void stop_connections();

    std::string address_;
    std::uint16_t port_;
    std::string input_path_;
    net::TcpSocket listener_;
    std::vector<std::unique_ptr<RtspServerConnection>> connections_;
    mutable std::mutex connections_mutex_;
    std::atomic<bool> stopping_{false};
};

}  // namespace rtsp
