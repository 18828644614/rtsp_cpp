#include "rtp/rtp_sender.h"

#include <stdexcept>
#include <utility>

namespace rtsp {

RtpSender::RtpSender(MediaTransport transport, int payload_type, std::uint16_t sequence,
                     std::uint32_t ssrc)
    : transport_(std::move(transport)), payload_type_(payload_type), sequence_(sequence),
      ssrc_(ssrc) {
    if (payload_type_ < 0 || payload_type_ > 127 || ssrc_ == 0) {
        throw std::invalid_argument("invalid RTP sender parameters");
    }
}

bool RtpSender::send_access_unit(const H264AccessUnit& access_unit, std::uint32_t timestamp,
                                 net::OutboundQueue& queue) {
    for (std::size_t nalu_index = 0; nalu_index < access_unit.nalus.size(); ++nalu_index) {
        const std::vector<std::string> payloads = packetizer_.packetize(access_unit.nalus[nalu_index]);
        for (std::size_t fragment = 0; fragment < payloads.size(); ++fragment) {
            RtpPacket packet;
            packet.payload_type = static_cast<unsigned int>(payload_type_);
            packet.sequence_number = sequence_;
            packet.timestamp = timestamp;
            packet.ssrc = ssrc_;
            packet.marker = nalu_index + 1 == access_unit.nalus.size()
                            && fragment + 1 == payloads.size();
            packet.payload = payloads[fragment];

            net::OutboundMessage message;
            message.kind = net::OutboundKind::Rtp;
            message.wire = transport_.frame_rtp(serialize_rtp_packet(packet));
            if (!queue.push(std::move(message))) {
                return false;
            }
            sequence_ = static_cast<std::uint16_t>(sequence_ + 1);
        }
    }
    return true;
}

std::uint16_t RtpSender::next_sequence() const {
    return sequence_;
}

std::uint32_t RtpSender::ssrc() const {
    return ssrc_;
}

}  // namespace rtsp
