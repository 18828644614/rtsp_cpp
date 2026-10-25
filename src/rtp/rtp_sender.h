#pragma once

#include "media/h264_file_reader.h"
#include "media/h264_packetizer.h"
#include "net/outbound_queue.h"
#include "rtp/rtp_packet.h"
#include "rtsp/media_transport.h"

#include <cstdint>

namespace rtsp {

// 把一帧 H264 视频数据转换成一个或多个 RTP 包，再封装成 RTSP over TCP 的交织帧，最后放入发送队列。
// 整体链路： H264AccessUnit  ->  H264Packetizer  ->  H264 RTP payload  ->  RtpPacket  ->  serialize_rtp_packet()  ->  MediaTransport::frame_rtp()
//  ->  OutboundMessage  ->  OutboundQueue  ->  send_loop()  ->  TcpSocket::send_all()

// H264AccessUnit：
// 一帧视频
//   ├── NALU 1
//   ├── NALU 2
//   └── NALU 3
class RtpSender {
public:
    // MediaTransport transport: 表示媒体传输方式以及 RTP 使用的通道, MediaTransport 中保存了 RTP 和 RTCP 的通道号
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

    // 负责把 H264 NALU 转换成 RTP 可以承载的负载
    H264Packetizer packetizer_;
};

}  // namespace rtsp
