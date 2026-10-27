#include "rtsp/rtsp_server.h"

#include "media/media_session.h"
#include "rtsp/rtsp_server_connection.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace rtsp {

RtspServer::RtspServer(std::string address, std::uint16_t port, std::string video_path,
                       std::string audio_path, RuntimeOptions options)
    : address_(std::move(address)), port_(port), options_(options),
      media_session_(std::make_shared<MediaSession>(video_path, audio_path)) {}

RtspServer::~RtspServer() {
    stop();
    // 调用者须先等待 run() 返回；对象不能在运行线程仍访问它时析构。
    close_connections();
}

void RtspServer::run(const std::atomic<bool>* stop_requested) {
    // 把 running_ 设置为 true
    // 返回设置之前的值
    if (running_.exchange(true)) {
        throw std::logic_error("server is already running");
    }
    stopping_.store(false);
    const auto should_stop = [&] {
        return stopping_.load() || (stop_requested && stop_requested->load());
    };
    try {
        listener_.listen(address_, port_);
        listener_.set_nonblocking();
        bound_port_.store(listener_.local_port());
        std::cout << "RTSP server listening on " << address_ << ':'
                  << bound_port_.load() << std::endl;
        while (!should_stop()) {
            const auto now = MediaSession::Clock::now();
            media_session_->tick(now);
            for (auto& connection : connections_) {
                connection->check_timeout(now);
            }
            reap_finished_connections();

            event_loop_.clear();
            // 监听socket
            event_loop_.watch(listener_.native_handle(), true, false);
            for (const auto& connection : connections_) {
                event_loop_.watch(connection->native_handle(),
                                  connection->wants_read(), connection->wants_write());
            }

            // 最多等待 50 ms，外部 stop() 只设标记，无需跨线程操作 Socket。
            int timeout_ms = 50;
            if (const auto deadline = media_session_->next_deadline()) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                    *deadline - MediaSession::Clock::now()).count();
                timeout_ms = static_cast<int>(std::clamp<long long>(remaining, 0, timeout_ms));
            }
            for (const auto& connection : connections_) {
                if (const auto deadline = connection->next_deadline()) {
                    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                        *deadline - MediaSession::Clock::now()).count();
                    timeout_ms = static_cast<int>(std::clamp<long long>(remaining, 0, timeout_ms));
                }
            }
            const auto events = event_loop_.wait(timeout_ms);
            if (should_stop()) {
                break;
            }
            for (const auto& event : events) {
                if (event.socket == listener_.native_handle()) {
                    // 限制每轮 accept 数量，避免连接洪峰挤占已有连接。
                    for (int accepted = 0; accepted < 16; ++accepted) {
                        auto connection = std::make_unique<RtspServerConnection>(media_session_, options_);
                        if (!connection->accept_from(listener_)) {
                            break;
                        }
                        if (connections_.size() >= net::EventLoop::capacity - 1) {
                            connection->close();  // 监听 Socket 也占一个 select 名额。
                            continue;
                        }
                        std::cout << "Client connected, Session=" << connection->session_id()
                                  << std::endl;
                        connections_.push_back(std::move(connection));
                        connection_count_.store(connections_.size());
                    }
                    continue;
                }
                for (auto& connection : connections_) {
                    if (connection->finished() || connection->native_handle() != event.socket) {
                        continue;
                    }
                    if (event.readable) {
                        connection->on_readable();
                    }
                    if (event.writable && !connection->finished()) {
                        connection->on_writable();
                    }
                    break;
                }
            }
            // 不在事件分派中销毁连接，避免同批事件引用失效。
            reap_finished_connections();
        }
    } catch (...) {
        listener_.close();
        close_connections();
        media_session_->stop();
        running_.store(false);
        throw;
    }
    try {
        listener_.close();
        media_session_->stop();
        // 停止接受和生产后继续驱动同一事件循环，排空最终 RTCP；每连接有绝对收尾期限。
        for (auto& connection : connections_) { connection->start_shutdown(); }
        while (!connections_.empty()) {
            for (auto& connection : connections_) { connection->check_timeout(MediaSession::Clock::now()); }
            reap_finished_connections();
            if (connections_.empty()) { break; }
            event_loop_.clear();
            for (const auto& connection : connections_) {
                event_loop_.watch(connection->native_handle(), false, connection->wants_write());
            }
            for (const auto& event : event_loop_.wait(50)) {
                for (auto& connection : connections_) {
                    if (connection->native_handle() == event.socket && event.writable) {
                        connection->on_writable();
                        break;
                    }
                }
            }
        }
    } catch (...) {
        close_connections();
        running_.store(false);
        throw;
    }
    running_.store(false);
}

void RtspServer::stop() {
    stopping_.store(true);
}

std::uint16_t RtspServer::local_port() const {
    return bound_port_.load();
}

std::size_t RtspServer::connection_count() const {
    return connection_count_.load();
}

void RtspServer::reap_finished_connections() {
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
        [](const auto& connection) { return connection->finished(); }), connections_.end());
    connection_count_.store(connections_.size());
}

void RtspServer::close_connections() {
    for (auto& connection : connections_) {
        connection->close();
    }
    connections_.clear();
    connection_count_.store(0);
}

}  // namespace rtsp
