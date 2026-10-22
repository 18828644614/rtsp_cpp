#include "media/h264_packetizer.h"

#include <algorithm>
#include <stdexcept>

namespace rtsp {

std::vector<std::string> H264Packetizer::packetize(const std::string& nalu,
                                                  std::size_t max_payload_size) const {
    if (nalu.empty() || max_payload_size < 3) {
        throw std::runtime_error("H264 requires a NALU and a payload limit of at least 3");
    }
    const unsigned int header = static_cast<unsigned char>(nalu[0]);
    const unsigned int type = header & 0x1f;
    if ((header & 0x80) != 0 || type == 0 || type > 23) {
        throw std::runtime_error("expected an original H264 NALU of type 1..23");
    }

    std::vector<std::string> payloads;
    if (nalu.size() <= max_payload_size) {
        payloads.push_back(nalu);
        return payloads;
    }

    // FU-A：保留 F/NRI，把类型改为 28；原始类型放入第二个字节。
    const unsigned int indicator = (header & 0xe0) | 28;
    const std::size_t max_fragment_size = max_payload_size - 2;
    std::size_t position = 1;  // 原始 NAL 头不作为分片数据发送。
    while (position < nalu.size()) {
        const std::size_t count = std::min(max_fragment_size, nalu.size() - position);
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
