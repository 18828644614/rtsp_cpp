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

bool RtpSender::send_payloads(const RtpPayloadUnit& unit, std::uint32_t timestamp,
                              net::OutboundQueue& queue) {
    if (unit.payloads.empty()) {
        throw std::invalid_argument("expected nonempty RTP payloads");
    }
    for (std::size_t index = 0; index < unit.payloads.size(); ++index) {
        RtpPacket packet;
        packet.payload_type = static_cast<unsigned int>(payload_type_);
        packet.sequence_number = sequence_;
        packet.timestamp = timestamp;
        packet.ssrc = ssrc_;
        packet.marker = index + 1 == unit.payloads.size();
        packet.payload = unit.payloads[index];

        net::OutboundMessage message;
        message.kind = net::OutboundKind::Rtp;
        message.wire = transport_.frame_rtp(serialize_rtp_packet(packet));
        if (!queue.push(std::move(message))) {
            return false;
        }
        sequence_ = static_cast<std::uint16_t>(sequence_ + 1);
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
