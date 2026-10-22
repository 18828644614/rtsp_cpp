#include "rtp/rtp_packet.h"

#include <stdexcept>

namespace rtsp {
namespace {

// char 在不同编译器中可能有符号，读二进制前先转成 unsigned char。
unsigned int byte_at(const std::string& data, std::size_t position) {
    return static_cast<unsigned char>(data[position]);
}

unsigned int read_u16(const std::string& data, std::size_t position) {
    return byte_at(data, position) * 256 + byte_at(data, position + 1);
}

std::uint32_t read_u32(const std::string& data, std::size_t position) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | byte_at(data, position + i);
    }
    return value;
}

void write_u16(std::string& data, unsigned int value) {
    data += static_cast<char>((value >> 8) & 0xff);
    data += static_cast<char>(value & 0xff);
}

void write_u32(std::string& data, std::uint32_t value) {
    data += static_cast<char>((value >> 24) & 0xff);
    data += static_cast<char>((value >> 16) & 0xff);
    data += static_cast<char>((value >> 8) & 0xff);
    data += static_cast<char>(value & 0xff);
}

}  // namespace

RtpPacket parse_rtp_packet(const std::string& wire) {
    if (wire.size() < 12) {
        throw std::runtime_error("RTP header is shorter than 12 bytes");
    }
    const unsigned int first = byte_at(wire, 0);
    if ((first >> 6) != 2) {
        throw std::runtime_error("unsupported RTP version");
    }

    RtpPacket packet;
    packet.marker = (byte_at(wire, 1) & 0x80) != 0;
    packet.payload_type = byte_at(wire, 1) & 0x7f;
    packet.sequence_number = static_cast<std::uint16_t>(read_u16(wire, 2));
    packet.timestamp = read_u32(wire, 4);
    packet.ssrc = read_u32(wire, 8);

    const unsigned int csrc_count = first & 0x0f;
    std::size_t position = 12;
    if (wire.size() - position < csrc_count * 4) {
        throw std::runtime_error("truncated RTP CSRC list");
    }
    for (unsigned int i = 0; i < csrc_count; ++i) {
        packet.csrcs.push_back(read_u32(wire, position));
        position += 4;
    }

    packet.has_extension = (first & 0x10) != 0;
    if (packet.has_extension) {
        if (wire.size() - position < 4) {
            throw std::runtime_error("truncated RTP extension header");
        }
        packet.extension_profile = read_u16(wire, position);
        // 扩展长度只计算扩展数据，单位是 32 位字，不包含前面的 4 字节。
        const std::size_t extension_size = read_u16(wire, position + 2) * 4;
        position += 4;
        if (wire.size() - position < extension_size) {
            throw std::runtime_error("truncated RTP extension data");
        }
        packet.extension_data = wire.substr(position, extension_size);
        position += extension_size;
    }

    std::size_t payload_end = wire.size();
    if ((first & 0x20) != 0) {
        packet.padding_size = byte_at(wire, wire.size() - 1);
        if (packet.padding_size == 0 || packet.padding_size > wire.size() - position) {
            throw std::runtime_error("invalid RTP padding size");
        }
        payload_end -= packet.padding_size;
    }
    packet.payload = wire.substr(position, payload_end - position);
    return packet;
}

std::string serialize_rtp_packet(const RtpPacket& packet) {
    if (packet.payload_type > 127 || packet.csrcs.size() > 15
        || packet.padding_size > 255 || packet.extension_profile > 65535) {
        throw std::runtime_error("RTP field is out of range");
    }
    if (!packet.has_extension
        && (!packet.extension_data.empty() || packet.extension_profile != 0)) {
        throw std::runtime_error("RTP extension data requires has_extension");
    }
    if (packet.extension_data.size() % 4 != 0 || packet.extension_data.size() / 4 > 65535) {
        throw std::runtime_error("RTP extension size must fit a 16-bit word count");
    }

    unsigned int first = 0x80;  // V=2，占最高两位。
    if (packet.padding_size != 0) {
        first |= 0x20;
    }
    if (packet.has_extension) {
        first |= 0x10;
    }
    first |= static_cast<unsigned int>(packet.csrcs.size());

    std::string wire;
    wire += static_cast<char>(first);
    wire += static_cast<char>((packet.marker ? 0x80 : 0) | packet.payload_type);
    write_u16(wire, packet.sequence_number);
    write_u32(wire, packet.timestamp);
    write_u32(wire, packet.ssrc);
    for (std::size_t i = 0; i < packet.csrcs.size(); ++i) {
        write_u32(wire, packet.csrcs[i]);
    }
    if (packet.has_extension) {
        write_u16(wire, packet.extension_profile);
        write_u16(wire, static_cast<unsigned int>(packet.extension_data.size() / 4));
        wire += packet.extension_data;
    }
    wire += packet.payload;
    if (packet.padding_size != 0) {
        wire.append(packet.padding_size - 1, '\0');
        wire += static_cast<char>(packet.padding_size);
    }
    return wire;
}

}  // namespace rtsp
