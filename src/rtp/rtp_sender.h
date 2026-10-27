#pragma once

#include "rtp/rtp_payload.h"
#include "net/outbound_queue.h"
#include "rtp/rtp_packet.h"

#include <cstdint>

namespace rtsp {

// 每客户端独有：给共享负载添加 RTP 头和传输头，再投递到该客户端的队列。
class RtpSender {
public:
    // 直接使用 SETUP 协商的 RTP 通道；RTCP 通道由订阅管理。
    RtpSender(int rtp_channel, int payload_type, std::uint16_t sequence,
              std::uint32_t ssrc);

    RtpSender(const RtpSender&) = delete;
    RtpSender& operator=(const RtpSender&) = delete;

    // 只生成并入队 RTP，不直接访问 Socket。失败表示输出队列已满或已关闭。
    bool send_payloads(const RtpPayloadUnit& unit, std::uint32_t timestamp,
                       net::OutboundQueue& queue);
    std::uint16_t next_sequence() const;
    std::uint32_t ssrc() const;

private:
    int rtp_channel_;
    int payload_type_;
    std::uint16_t sequence_;
    std::uint32_t ssrc_;

};

}  // namespace rtsp
