#pragma once

#include "net/tcp_socket.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace rtsp {

class RtspServerConnection {
public:
    explicit RtspServerConnection(std::string input_path);
    ~RtspServerConnection();

    RtspServerConnection(const RtspServerConnection&) = delete;
    RtspServerConnection& operator=(const RtspServerConnection&) = delete;

    void accept_from(net::TcpSocket& listener);
    void start();
    void stop();
    void join();

    bool finished() const;
    const std::string& session_id() const;

private:
    void run();

    net::TcpSocket socket_;
    std::string input_path_;
    std::string session_id_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};
};

}  // namespace rtsp
