#pragma once

#include "net/outbound_queue.h"
#include "net/tcp_socket.h"
#include "rtsp/rtsp_server_session.h"
#include "rtsp/rtsp_stream_parser.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

namespace rtsp {

class MediaSession;
class MediaSubscription;

// 一个连接只保存状态；所有操作都在服务器的事件循环线程上执行。
class RtspServerConnection {
public:
    using Clock = std::chrono::steady_clock;
    explicit RtspServerConnection(std::shared_ptr<MediaSession> media_session);
    ~RtspServerConnection();

    RtspServerConnection(const RtspServerConnection&) = delete;
    RtspServerConnection& operator=(const RtspServerConnection&) = delete;

    bool accept_from(net::TcpSocket& listener);
    void on_readable();
    void on_writable();

    // 检查这个连接是否还能继续工作、是否该进入收尾，以及是否因为长时间没有进展而需要关闭。
    void check_timeout(Clock::time_point now);
    void close();

    bool wants_read() const;
    bool wants_write() const;
    bool finished() const;
    SOCKET native_handle() const;
    const std::string& session_id() const;

private:
    enum class State { Open, Draining, Closed };
    void handle_item(const RtspStreamItem& item);
    void begin_drain();
    void fail(const std::exception& error);

    net::TcpSocket socket_;
    std::shared_ptr<MediaSession> media_session_;
    std::string session_id_;
    std::shared_ptr<net::OutboundQueue> outbound_queue_;
    // RTSP 协议状态、轨道 SETUP 信息等
    RtspServerSession session_;
    RtspStreamParser parser_;
    // 当前连接的媒体订阅
    std::shared_ptr<MediaSubscription> subscription_;
    State state_ = State::Open;
    // 当前正在发送的消息
    net::OutboundMessage sending_;
    bool has_sending_ = false;
    // 当前消息已经发送了多少字节
    std::size_t send_offset_ = 0;
    Clock::time_point last_read_ = Clock::now();
    Clock::time_point last_progress_ = Clock::now();
    bool waiting_write_ = false;
};

}  // namespace rtsp
