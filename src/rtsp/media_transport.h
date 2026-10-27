#pragma once

#include "rtsp/rtsp_session_utils.h"

#include <string>

namespace rtsp {

enum class TransportMode {
    InterleavedTcp
};

// 统一传输值对象。实际写 Socket 由事件循环中的连接 on_writable 负责。
class MediaTransport {
public:
    explicit MediaTransport(TcpTransport tcp_transport);

    TransportMode mode() const;
    const TcpTransport& tcp() const;
    std::string frame_rtp(const std::string& payload) const;
    std::string frame_rtcp(const std::string& payload) const;

private:
    TransportMode mode_ = TransportMode::InterleavedTcp;
    TcpTransport tcp_;
};

}  // namespace rtsp
