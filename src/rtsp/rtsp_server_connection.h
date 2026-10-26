#pragma once

#include "net/outbound_queue.h"
#include "net/tcp_socket.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace rtsp {

class MediaSession;

class RtspServerConnection {
public:
    // 1.保存共享媒体会话, 2.生成这个连接的会话 ID, 3.创建这个连接自己的发送队列
    explicit RtspServerConnection(std::shared_ptr<MediaSession> media_session);
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
    // 它是一个“发送线程”，主要负责把输出队列中的数据发送给客户端
    void send_loop();

    net::TcpSocket socket_;
    std::shared_ptr<MediaSession> media_session_;
    std::string session_id_;
    std::shared_ptr<net::OutboundQueue> outbound_queue_;
    std::thread worker_;
    std::thread sender_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> sender_finished_{false};
};

}  // namespace rtsp
