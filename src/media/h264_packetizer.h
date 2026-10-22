#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rtsp {

class H264Packetizer {
public:
    // 输入一个 NALU：包含 NAL 头，不包含 Annex-B 起始码。
    // 输出各 RTP 包中的 H.264 负载，不包含 RTP 头或 TCP 交织头。
    // max_payload_size 包含 FU-A 自己的两字节头，必须至少为 3。
    std::vector<std::string> packetize(const std::string& nalu,
                                       std::size_t max_payload_size = 1200) const;
};

}  // namespace rtsp
