#include "rtsp/rtsp_server.h"

#include "media/media_session.h"
#include "rtsp/rtsp_server_connection.h"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace rtsp {

RtspServer::RtspServer(std::string address, std::uint16_t port, std::string video_path,
                       std::string audio_path)
    : address_(std::move(address)), port_(port),
      media_session_(std::make_shared<MediaSession>(video_path, audio_path)) {}

RtspServer::~RtspServer() {
    stop();
    stop_connections();
    listener_.close();
}

void RtspServer::run() {
    if (listener_.is_open()) {
        throw std::logic_error("server is already running");
    }
    stopping_.store(false, std::memory_order_release);
    listener_.listen(address_, port_);
    std::cout << "RTSP server listening on " << address_ << ':'
              << listener_.local_port() << std::endl;

    try {
        while (!stopping_.load(std::memory_order_acquire)) {
            // 删除已经断开的连接
            reap_finished_connections();
            if (!listener_.wait_readable(100)) {
                continue;
            }

            // 创建一个新的客户端连接对象，让它使用已经存在的媒体会话，再用 unique_ptr 管理这个连接对象
            auto connection = std::make_unique<RtspServerConnection>(media_session_);
            //  连接 A
            //   ├── media_session_ ───────┐
            //   └── outbound_queue_ → 队列 A
            //                            │
            //                            ▼
            //                     同一个 MediaSession
            //                            ▲
            //                            │
            // 连接 B                     │
            //   ├── media_session_ ───────┘
            //   └── outbound_queue_ → 队列 B

            // 从监听 Socket 接收一个客户端连接
            connection->accept_from(listener_);
            std::cout << "Client connected, Session=" << connection->session_id() << std::endl;
            connection->start();
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                if (stopping_.load(std::memory_order_acquire)) {
                    connection->stop();
                }
                // 把连接对象的所有权交给服务器的连接容器
                connections_.push_back(std::move(connection));
            }
        }
    } catch (const std::exception&) {
        if (!stopping_.load(std::memory_order_acquire)) {
            stop_connections();
            listener_.close();
            media_session_->stop();
            throw;
        }
    }

    listener_.close();
    stop_connections();
    media_session_->stop();
}

void RtspServer::stop() {
    stopping_.store(true, std::memory_order_release);
    // 监听线程最多 100 ms 后观察停止标志，再由该线程关闭监听句柄。
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        connections_[index]->stop();
    }
}

std::uint16_t RtspServer::local_port() const {
    return listener_.local_port();
}

std::size_t RtspServer::connection_count() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return connections_.size();
}

void RtspServer::reap_finished_connections() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    std::size_t index = 0;
    while (index < connections_.size()) {
        if (!connections_[index]->finished()) {
            ++index;
            continue;
        }
        connections_[index]->join();
        connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void RtspServer::stop_connections() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        connections_[index]->stop();
    }
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        connections_[index]->join();
    }
    connections_.clear();
}

}  // namespace rtsp
