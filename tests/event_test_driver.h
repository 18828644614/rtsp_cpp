#pragma once

#include "media/media_session.h"
#include "net/event_loop.h"
#include "rtsp/rtsp_server_connection.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// 测试显式推进所有对象，替代旧测试隐式依赖的后台线程；生产代码不使用此驱动器。
namespace test {

inline std::vector<rtsp::MediaSession*> media_sessions;
inline std::vector<rtsp::RtspServerConnection*> connections;

class DrivenMedia : public rtsp::MediaSession {
public:
    explicit DrivenMedia(const std::string& video, const std::string& audio = "")
        : rtsp::MediaSession(video, audio) {
        media_sessions.push_back(this);
    }
    ~DrivenMedia() {
        media_sessions.erase(std::remove(media_sessions.begin(), media_sessions.end(), this),
                             media_sessions.end());
    }
};

class DrivenConnection : public rtsp::RtspServerConnection {
public:
    explicit DrivenConnection(std::shared_ptr<rtsp::MediaSession> media, rtsp::RuntimeOptions options = {})
        : rtsp::RtspServerConnection(std::move(media), options) {
        connections.push_back(this);
    }
    ~DrivenConnection() {
        connections.erase(std::remove(connections.begin(), connections.end(), this),
                          connections.end());
    }
};

inline void pump() {
    const auto now = rtsp::MediaSession::Clock::now();
    for (auto* media : media_sessions) {
        media->tick(now);
    }
    rtsp::net::EventLoop loop;
    bool watched = false;
    for (auto* connection : connections) {
        connection->check_timeout(now);
        if (!connection->finished() && connection->native_handle() != INVALID_SOCKET) {
            loop.watch(connection->native_handle(), connection->wants_read(), connection->wants_write());
            watched = true;
        }
    }
    if (!watched) {
        return;
    }
    for (const auto& event : loop.wait(0)) {
        for (auto* connection : connections) {
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
}

inline void wait_finished(rtsp::RtspServerConnection& connection) {
    const auto deadline = rtsp::MediaSession::Clock::now() + std::chrono::seconds(3);
    while (!connection.finished()) {
        if (rtsp::MediaSession::Clock::now() >= deadline) {
            throw std::runtime_error("event connection did not finish");
        }
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace test
