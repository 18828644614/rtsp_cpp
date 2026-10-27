#pragma once

#include "net/tcp_socket.h"
#include "net/event_loop.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rtsp {

class RtspServerConnection;
class MediaSession;

class RtspServer {
public:
    RtspServer(std::string address, std::uint16_t port, std::string video_path,
               std::string audio_path = "");
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    void run();
    void stop();

    std::uint16_t local_port() const;
    std::size_t connection_count() const;

private:
    void reap_finished_connections();
    void close_connections();

    std::string address_;
    std::uint16_t port_;
    std::shared_ptr<MediaSession> media_session_;
    net::TcpSocket listener_;
    net::EventLoop event_loop_;
    std::vector<std::unique_ptr<RtspServerConnection>> connections_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> bound_port_{0};
    std::atomic<std::size_t> connection_count_{0};
};

}  // namespace rtsp
