#include "media/h264_depacketizer.h"

#include <stdexcept>

namespace rtsp {
namespace {

// 限制一个完整 NALU 的大小，避免异常分片一直累计。
const std::size_t kMaxNalSize = 4 * 1024 * 1024;

unsigned int byte_at(const std::string& data, std::size_t position) {
    return static_cast<unsigned char>(data[position]);
}

void check_nalu(const std::string& nalu) {
    if (nalu.empty() || nalu.size() > kMaxNalSize) {
        throw std::runtime_error("H264 NALU is empty or larger than 4 MiB");
    }
    const unsigned int header = byte_at(nalu, 0);
    const unsigned int type = header & 0x1f;
    if ((header & 0x80) != 0 || type == 0 || type > 23) {
        throw std::runtime_error("invalid original H264 NALU header");
    }
}

std::vector<std::string> unpack_stap_a(const std::string& payload) {
    std::vector<std::string> nalus;
    std::size_t position = 1;  // 跳过 STAP-A 的类型字节。
    while (position < payload.size()) {
        if (payload.size() - position < 2) {
            throw std::runtime_error("truncated STAP-A NALU length");
        }
        const std::size_t size = byte_at(payload, position) * 256 + byte_at(payload, position + 1);
        position += 2;
        if (size == 0 || size > payload.size() - position) {
            throw std::runtime_error("invalid STAP-A NALU length");
        }
        const std::string nalu = payload.substr(position, size);
        check_nalu(nalu);
        nalus.push_back(nalu);
        position += size;
    }
    if (nalus.empty()) {
        throw std::runtime_error("STAP-A contains no NALUs");
    }
    return nalus;
}

}  // namespace

void H264Depacketizer::reset() {
    assembling_ = false;
    nalu_.clear();
    next_sequence_ = 0;
    timestamp_ = 0;
    ssrc_ = 0;
    payload_type_ = 0;
    nal_header_ = 0;
}

H264DepacketizeResult H264Depacketizer::push(const RtpPacket& packet,
                                            std::vector<std::string>& nalus) {
    nalus.clear();
    try {
        if (packet.payload.empty()) {
            throw std::runtime_error("empty H264 RTP payload");
        }
        const unsigned int header = byte_at(packet.payload, 0);
        const unsigned int type = header & 0x1f;
        if ((header & 0x80) != 0) {
            throw std::runtime_error("H264 forbidden_zero_bit must be zero");
        }
        if (type >= 1 && type <= 23) {
            check_nalu(packet.payload);
            reset();  // 新的完整 NALU 到达时，放弃尚未完成的旧分片。
            nalus.push_back(packet.payload);
            return H264Complete;
        }
        if (type == 24) {
            // 局部结果全部验证后再交给调用方，避免输出半个 STAP-A。
            const std::vector<std::string> complete = unpack_stap_a(packet.payload);
            reset();
            nalus = complete;
            return H264Complete;
        }
        if (type == 28) {
            return push_fu_a(packet, nalus);
        }
        throw std::runtime_error("unsupported H264 RTP packet type");
    } catch (...) {
        reset();
        nalus.clear();
        throw;  // 保留原错误，调用方决定继续收包还是结束会话。
    }
}

H264DepacketizeResult H264Depacketizer::push_fu_a(const RtpPacket& packet,
                                                 std::vector<std::string>& nalus) {
    const std::string& payload = packet.payload;
    if (payload.size() < 2) {
        throw std::runtime_error("truncated FU-A header");
    }
    const unsigned int fragment_header = byte_at(payload, 1);
    const unsigned int type = fragment_header & 0x1f;
    const bool start = (fragment_header & 0x80) != 0;
    const bool end = (fragment_header & 0x40) != 0;
    // RFC 6184 要求接收端忽略 R 位（0x20），发送端保持为 0。
    if (type == 0 || type > 23 || (start && end)) {
        throw std::runtime_error("invalid FU-A header");
    }
    const unsigned int nal_header = (byte_at(payload, 0) & 0xe0) | type;

    if (start) {
        reset();  // 新开始片允许从之前的缺片中恢复。
        assembling_ = true;
        nalu_ += static_cast<char>(nal_header);
        timestamp_ = packet.timestamp;
        ssrc_ = packet.ssrc;
        payload_type_ = packet.payload_type;
        nal_header_ = nal_header;
    } else if (!assembling_ || packet.sequence_number != next_sequence_
               || packet.timestamp != timestamp_ || packet.ssrc != ssrc_
               || packet.payload_type != payload_type_ || nal_header != nal_header_) {
        reset();
        return H264Dropped;
    }

    // FU-A 可携带空分片；仍检查顺序，并依靠总大小上限限制累计数据。
    const std::size_t fragment_size = payload.size() - 2;
    if (fragment_size > kMaxNalSize - nalu_.size()) {
        throw std::runtime_error("assembled H264 NALU is larger than 4 MiB");
    }
    nalu_.append(payload, 2, fragment_size);
    next_sequence_ = static_cast<std::uint16_t>(packet.sequence_number + 1);
    if (!end) {
        return H264Waiting;
    }
    nalus.push_back(nalu_);
    reset();
    return H264Complete;
}

}  // namespace rtsp
