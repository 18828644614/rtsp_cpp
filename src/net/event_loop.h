#pragma once

#include "net/socket_utils.h"

#include <cstddef>
#include <vector>

namespace rtsp::net {

struct SocketEvent {
    SOCKET socket = INVALID_SOCKET;
    bool readable = false;
    bool writable = false;
};

// 只封装就绪等待。对象所有权、媒体期限和事件分派由 RtspServer 管理。
class EventLoop {
public:
    static constexpr std::size_t capacity = FD_SETSIZE;
    // 清空上一轮的关注集合
    void clear();
    // 登记这一轮关注的读写事件
    void watch(SOCKET socket, bool read, bool write);
    // 调用 select()，返回已经就绪的 Socket
    std::vector<SocketEvent> wait(int timeout_ms) const;

private:
    std::vector<SocketEvent> interests_;
};

}  // namespace rtsp::net
