#pragma once

#include "rtp/rtp_packet.h"

namespace rtsp {

enum H264DepacketizeResult {
    H264Waiting,   // FU-A 尚未收完，输出为空。
    H264Complete,  // 输出一个或多个完整 NALU。
    H264Dropped   // 缺少开始片或连续性被破坏，丢弃未完成 NALU。
};

// 一个对象用于一路 H.264 轨道，不负责重排乱序包或识别图像边界。
class H264Depacketizer {
public:
    // 每次先清空 nalus；格式错误时抛异常并清空内部重组状态。
    // 单 NALU / STAP-A 立即输出，FU-A 只在结束片到达后输出。
    H264DepacketizeResult push(const RtpPacket& packet, std::vector<std::string>& nalus);
    void reset();

private:
    H264DepacketizeResult push_fu_a(const RtpPacket& packet, std::vector<std::string>& nalus);

    bool assembling_ = false;
    std::string nalu_;
    std::uint16_t next_sequence_ = 0;
    std::uint32_t timestamp_ = 0;
    std::uint32_t ssrc_ = 0;
    unsigned int payload_type_ = 0;
    unsigned int nal_header_ = 0;
};

}  // namespace rtsp
