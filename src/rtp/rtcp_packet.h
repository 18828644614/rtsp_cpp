#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rtsp {

struct RtcpReceptionReport {
    std::uint32_t ssrc = 0;
    unsigned int fraction_lost = 0;
    std::int32_t cumulative_lost = 0;
    std::uint32_t highest_sequence = 0;
    std::uint32_t jitter = 0;
    std::uint32_t last_sr = 0;
    std::uint32_t delay_since_last_sr = 0;
};

struct RtcpPacket {
    unsigned int type = 0;
    std::uint32_t ssrc = 0;
    std::uint64_t ntp = 0;
    std::uint32_t rtp_timestamp = 0;
    std::uint32_t packet_count = 0;
    std::uint32_t octet_count = 0;
    std::vector<RtcpReceptionReport> reports;
    std::vector<std::uint32_t> bye_sources;
    std::string cname;
};

// SR + SDES(CNAME)，正常结束时追加 BYE；所有长度按网络字节序编码。
std::string make_rtcp_sender_report(std::uint32_t ssrc, std::uint64_t ntp,
                                    std::uint32_t rtp_timestamp, std::uint32_t packets,
                                    std::uint32_t octets, const std::string& cname,
                                    bool bye = false);

// 完整校验 compound 中的每个包；截断、错误版本/长度/填充均抛异常。
std::vector<RtcpPacket> parse_rtcp_compound(const std::string& wire);

}  // namespace rtsp
