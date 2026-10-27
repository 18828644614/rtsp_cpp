#include "media/h264_packetizer.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rtsp {

RtpPayloadUnit H264Packetizer::packetize_access_unit(const H264AccessUnit& access_unit) const {
    if (access_unit.nalus.empty()) {
        throw std::invalid_argument("expected a nonempty H264 access unit");
    }
    RtpPayloadUnit result;
    result.pts = access_unit.pts;
    result.clock_rate = 90000;
    result.is_keyframe = access_unit.is_keyframe;
    for (const std::string& nalu : access_unit.nalus) {
        for (std::string& payload : packetize(nalu)) {
            result.payloads.push_back(std::move(payload));
        }
    }
    return result;
}

std::vector<std::string> H264Packetizer::packetize(const std::string& nalu,
                                                  std::size_t max_payload_size) const {
    //1字节 FU indicator 1字节 FU header 至少 1字节数据 (3字节)
    if (nalu.empty() || max_payload_size < 3) {
        throw std::runtime_error("H264 requires a NALU and a payload limit of at least 3");
    }
    const unsigned int header = static_cast<unsigned char>(nalu[0]);

    // bit 7    bit 6-5    bit 4-0
    // F        NRI        Type
    const unsigned int type = header & 0x1f;
    if ((header & 0x80) != 0 || type == 0 || type > 23) {
        throw std::runtime_error("expected an original H264 NALU of type 1..23");
    }

    //数据较小的nalu直接返回
    std::vector<std::string> payloads;
    if (nalu.size() <= max_payload_size) {
        payloads.push_back(nalu);
        return payloads;
    }

    // FU-A：保留 F/NRI，把类型改为 28；原始类型放入第二个字节。
    // FU indicator：
    // F     | NRI     | Type=28
    // 1bit    2bit       5bit
    // FU header：
    // S     | E     | R     | 原始 Type
    // 1bit   1bit     1bit     5bit

    // 0xe0 = 1110 0000
    // 28   = 0001 1100
    // 保留原始 F,NRI
    const unsigned int indicator = (header & 0xe0) | 28;
    const std::size_t max_fragment_size = max_payload_size - 2;
    std::size_t position = 1;  // 原始 NAL 头不作为分片数据发送。
    while (position < nalu.size()) {
        const std::size_t count = std::min(max_fragment_size, nalu.size() - position);
        // type: 000 type
        unsigned int fragment_header = type;
        if (position == 1) {
            fragment_header |= 0x80;  // S：开始片。
        }
        if (position + count == nalu.size()) {
            fragment_header |= 0x40;  // E：结束片，与 RTP Marker 含义不同。
        }
        std::string payload;
        payload += static_cast<char>(indicator);
        payload += static_cast<char>(fragment_header);
        payload.append(nalu, position, count);
        payloads.push_back(payload);
        position += count;
    }
    return payloads;
}

}  // namespace rtsp
