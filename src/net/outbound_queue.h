#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace rtsp::net {

enum class OutboundKind {
    RtspResponse,
    Rtp,
    Rtcp
};

struct OutboundMessage {
    OutboundKind kind = OutboundKind::RtspResponse;
    std::string wire;
};

// 每个连接独有的有界输出队列。push 不等待空间，避免慢客户端阻塞媒体生产者。
class OutboundQueue {
public:
    explicit OutboundQueue(std::size_t max_bytes = 4 * 1024 * 1024,
                           std::size_t max_messages = 0);

    OutboundQueue(const OutboundQueue&) = delete;
    OutboundQueue& operator=(const OutboundQueue&) = delete;

    bool push(OutboundMessage message);
    bool pop(OutboundMessage& message);
    void close();
    bool closed() const;
    std::size_t bytes() const;

private:
    const std::size_t max_bytes_;
    const std::size_t max_messages_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<OutboundMessage> messages_;
    std::size_t bytes_ = 0;
    bool closed_ = false;
};

}  // namespace rtsp::net
