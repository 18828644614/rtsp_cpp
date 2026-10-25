#pragma once

#include "media/h264_file_reader.h"
#include "media/h264_packetizer.h"
#include "net/outbound_queue.h"
#include "rtp/rtp_packet.h"
#include "rtsp/media_transport.h"

#include <cstdint>

namespace rtsp {

class RtpSender {
public:
    RtpSender(MediaTransport transport, int payload_type, std::uint16_t sequence,
              std::uint32_t ssrc);

    RtpSender(const RtpSender&) = delete;
    RtpSender& operator=(const RtpSender&) = delete;

    // 只生成并入队 RTP，不直接访问 Socket。失败表示输出队列已满或已关闭。
    bool send_access_unit(const H264AccessUnit& access_unit, std::uint32_t timestamp,
                          net::OutboundQueue& queue);
    std::uint16_t next_sequence() const;
    std::uint32_t ssrc() const;

private:
    MediaTransport transport_;
    int payload_type_;
    std::uint16_t sequence_;
    std::uint32_t ssrc_;
    H264Packetizer packetizer_;
};

}  // namespace rtsp
