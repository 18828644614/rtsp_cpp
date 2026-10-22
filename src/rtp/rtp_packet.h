#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rtsp {

// RTP 的公共头和媒体负载。此结构不解释 payload 中的 H.264 / AAC 数据。
struct RtpPacket {
    bool marker = false;
    unsigned int payload_type = 96;
    std::uint16_t sequence_number = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
    std::vector<std::uint32_t> csrcs;

    bool has_extension = false;
    unsigned int extension_profile = 0;
    std::string extension_data;  // 长度必须是 4 字节的整数倍。
    unsigned int padding_size = 0;  // 含最后一个“填充长度”字节，0 表示不填充。
    std::string payload;  // 已剥离公共头、扩展头和 Padding 的媒体负载。
};

// 输入必须是一整个 RTP 包（不含 TCP 的 '$' 交织头）。
// 与 TCP 流解析不同，RTP 长度不足属于非法包，直接抛出异常。
RtpPacket parse_rtp_packet(const std::string& wire);

// 固定生成 RTP 版本 2，所有多字节数均使用网络字节序。
std::string serialize_rtp_packet(const RtpPacket& packet);

}  // namespace rtsp
