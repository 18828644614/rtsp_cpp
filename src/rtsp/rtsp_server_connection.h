#pragma once

#include "net/outbound_queue.h"
#include "net/tcp_socket.h"
#include "rtsp/rtsp_server_session.h"
#include "rtsp/rtsp_stream_parser.h"
#include "rtsp/runtime_options.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>

namespace rtsp {

class MediaSession;
class MediaSubscription;

// 一个连接只保存状态；所有操作都在服务器的事件循环线程上执行。
class RtspServerConnection {
public:
    using Clock = std::chrono::steady_clock;
    // 这行代码的作用是：创建一个管理单个客户端连接的对象，准备好它的会话状态、发送队列和超时配置。
    explicit RtspServerConnection(std::shared_ptr<MediaSession> media_session,
                                  RuntimeOptions options = {});
    ~RtspServerConnection();

    RtspServerConnection(const RtspServerConnection&) = delete;
    RtspServerConnection& operator=(const RtspServerConnection&) = delete;

    bool accept_from(net::TcpSocket& listener);
    // 从 Socket 读取数据 → 解析请求或 RTCP → 处理
    void on_readable();
    // 从发送队列取数据 → 发送到 Socket
    void on_writable();

    // 检查这个连接是否还能继续工作、是否该进入收尾，以及是否因为长时间没有进展而需要关闭。
    void check_timeout(Clock::time_point now);
    void close();
    void start_shutdown();
    std::optional<Clock::time_point> next_deadline() const;

    bool wants_read() const;
    // 有正在发送但尚未发完的消息
    // 或者
    // 队列中还有待发消息
    bool wants_write() const;
    bool finished() const;
    SOCKET native_handle() const;
    const std::string& session_id() const;

private:
    // | `Open` | 正常工作，可以接收请求、发送数据 |
    // | `Draining` | 正在收尾，停止接收请求，尽量发完已有数据 |
    // | `Closed` | Socket 已关闭 |
    enum class State { Open, Draining, Closed };
    void handle_item(const RtspStreamItem& item);
    void begin_drain(Clock::time_point now = Clock::now());
    void fail(const std::exception& error);

    net::TcpSocket socket_;
    std::shared_ptr<MediaSession> media_session_;
    std::string session_id_;
    std::shared_ptr<net::OutboundQueue> outbound_queue_;
    // RTSP 协议状态、轨道 SETUP 信息等
    RtspServerSession session_;
    RtspStreamParser parser_;
    RuntimeOptions options_;
    std::optional<Clock::time_point> session_deadline_;
    std::optional<Clock::time_point> drain_deadline_;
    // 当前连接的媒体订阅
    std::shared_ptr<MediaSubscription> subscription_;
    State state_ = State::Open;
    // 当前正在发送的消息
    net::OutboundMessage sending_;
    bool has_sending_ = false;
    // 当前消息已经发送了多少字节
    std::size_t send_offset_ = 0;
    Clock::time_point last_read_ = Clock::now();
    // 最近一次发送取得进展的时间
    Clock::time_point last_progress_ = Clock::now();
    // 当前是否正在跟踪一段待发送过程
    bool waiting_write_ = false;
};

}  // namespace rtsp
