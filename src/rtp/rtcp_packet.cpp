#include "rtp/rtcp_packet.h"

#include <stdexcept>

namespace rtsp {
namespace {

void put32(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out += static_cast<char>((value >> shift) & 255);
    }
}

unsigned int byte(const std::string& wire, std::size_t pos) {
    return static_cast<unsigned char>(wire[pos]);
}

std::uint32_t get32(const std::string& wire, std::size_t pos) {
    return (byte(wire, pos) << 24) | (byte(wire, pos + 1) << 16)
           | (byte(wire, pos + 2) << 8) | byte(wire, pos + 3);
}

void require(bool valid) {
    if (!valid) {
        throw std::runtime_error("malformed RTCP packet");
    }
}

}  // namespace

std::string make_rtcp_sender_report(std::uint32_t ssrc, std::uint64_t ntp,
                                    std::uint32_t rtp_timestamp, std::uint32_t packets,
                                    std::uint32_t octets, const std::string& cname, bool bye) {
    if (ssrc == 0 || cname.empty() || cname.size() > 255) {
        throw std::invalid_argument("invalid RTCP SSRC or CNAME");
    }
    std::string out("\x80\xc8\0\x06", 4);
    put32(out, ssrc);
    put32(out, static_cast<std::uint32_t>(ntp >> 32));
    put32(out, static_cast<std::uint32_t>(ntp));
    put32(out, rtp_timestamp);
    put32(out, packets);
    put32(out, octets);

    std::string chunk;
    put32(chunk, ssrc);
    chunk += '\x01';  // CNAME item.
    chunk += static_cast<char>(cname.size());
    chunk += cname;
    chunk += '\0';  // END item，补到 32 位边界。
    while (chunk.size() % 4 != 0) { chunk += '\0'; }
    out.append("\x81\xca", 2);
    const auto length = chunk.size() / 4;
    out += static_cast<char>(length >> 8);
    out += static_cast<char>(length & 255);
    out += chunk;
    if (bye) {
        out.append("\x81\xcb\0\x01", 4);
        put32(out, ssrc);
    }
    return out;
}

std::vector<RtcpPacket> parse_rtcp_compound(const std::string& wire) {
    require(!wire.empty());
    std::vector<RtcpPacket> packets;
    std::size_t offset = 0;
    while (offset < wire.size()) {
        require(wire.size() - offset >= 4 && (byte(wire, offset) >> 6) == 2);
        const unsigned int count = byte(wire, offset) & 31;
        const std::size_t size = ((byte(wire, offset + 2) << 8) + byte(wire, offset + 3) + 1) * 4;
        require(size <= wire.size() - offset);
        std::size_t end = offset + size;
        if ((byte(wire, offset) & 32) != 0) {
            const auto padding = byte(wire, end - 1);
            require(end == wire.size() && padding != 0 && padding <= size - 4);
            end -= padding;
        }
        const std::size_t body = offset + 4;
        RtcpPacket packet;
        packet.type = byte(wire, offset + 1);
        if (packet.type == 200 || packet.type == 201) {
            const std::size_t fixed_size = packet.type == 200 ? 28 : 8;
            require(end - offset >= fixed_size + count * 24);
            packet.ssrc = get32(wire, body);
            if (packet.type == 200) {
                packet.ntp = (static_cast<std::uint64_t>(get32(wire, body + 4)) << 32)
                             | get32(wire, body + 8);
                packet.rtp_timestamp = get32(wire, body + 12);
                packet.packet_count = get32(wire, body + 16);
                packet.octet_count = get32(wire, body + 20);
            }
            for (unsigned int i = 0; i < count; ++i) {
                const auto pos = offset + fixed_size + i * 24;
                RtcpReceptionReport report;
                report.ssrc = get32(wire, pos);
                report.fraction_lost = byte(wire, pos + 4);
                const auto lost = get32(wire, pos + 4) & 0xffffff;
                report.cumulative_lost = static_cast<std::int32_t>(lost)
                                         - ((lost & 0x800000) ? 0x1000000 : 0);
                report.highest_sequence = get32(wire, pos + 8);
                report.jitter = get32(wire, pos + 12);
                report.last_sr = get32(wire, pos + 16);
                report.delay_since_last_sr = get32(wire, pos + 20);
                packet.reports.push_back(report);
            }
        } else if (packet.type == 202) {
            std::size_t pos = body;
            for (unsigned int i = 0; i < count; ++i) {
                require(end - pos >= 4);
                const auto source = get32(wire, pos);
                pos += 4;
                bool ended = false;
                while (pos < end) {
                    const auto type = byte(wire, pos++);
                    if (type == 0) {
                        while ((pos - offset) % 4 != 0) {
                            require(pos < end && byte(wire, pos) == 0);
                            ++pos;
                        }
                        ended = true;
                        break;
                    }
                    require(pos < end);
                    const auto length = byte(wire, pos++);
                    require(length <= end - pos);
                    if (type == 1 && i == 0) {
                        packet.ssrc = source;
                        packet.cname = wire.substr(pos, length);
                    }
                    pos += length;
                }
                require(ended);
            }
            require(pos == end);
        } else if (packet.type == 203) {
            require(count != 0 && end - body >= count * 4);
            for (unsigned int i = 0; i < count; ++i) {
                packet.bye_sources.push_back(get32(wire, body + i * 4));
            }
            const auto pos = body + count * 4;
            if (pos < end) {
                const auto reason_size = byte(wire, pos);
                require(reason_size <= end - pos - 1 && end - pos - 1 - reason_size <= 3);
                for (auto pad = pos + 1 + reason_size; pad < end; ++pad) {
                    require(byte(wire, pad) == 0);
                }
            }
        }
        packets.push_back(std::move(packet));
        offset += size;
    }
    return packets;
}

}  // namespace rtsp
