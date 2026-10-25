#pragma once

#include "rtsp/rtsp_session_utils.h"

#include <string>

namespace rtsp {

enum class TransportMode {
    InterleavedTcp
};

// 阶段 2 的统一传输值对象。实际写 Socket 仍由连接的唯一发送线程负责。
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
