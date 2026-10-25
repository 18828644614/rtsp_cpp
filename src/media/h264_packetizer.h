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
    // NALU 足够小：直接作为一个 RTP 负载返回。
    // NALU 超过大小限制：按 FU-A 格式拆成多个负载返回。
    // 通过这个函数，得到 一个或多个 H.264负载 (较小的为1个，较多的 FU-A 分片有多个)

    // 一个访问单元 frame
    //     ↓ 包含多个 NALU
    // 每个 NALU 调用 packetize()
    //     ↓ 得到一个或多个 H.264 负载
    // 每个负载添加 RTP 头
    //     ↓ 得到 RTP 包
    // 每个 RTP 包添加 TCP 交织头
    //     ↓ 通过 TCP 发送
    std::vector<std::string> packetize(const std::string& nalu,
                                       std::size_t max_payload_size = 1200) const;
};

}  // namespace rtsp
