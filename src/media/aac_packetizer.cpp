#include "media/aac_packetizer.h"

#include <stdexcept>
#include <utility>

namespace rtsp {

RtpPayloadUnit AacPacketizer::packetize(
    const AacAccessUnit& frame, int sample_rate) const {
    if (frame.data.empty() || frame.data.size() > 8191 || sample_rate <= 0) {
        throw std::invalid_argument("invalid AAC AU size or sample rate");
    }
    // AU-headers-length = 16 bit；AU-size = 13 bit 字节数；AU-index = 0。
    const unsigned int size = static_cast<unsigned int>(frame.data.size());
    std::string payload("\0\x10", 2);
    payload += static_cast<char>(size >> 5);
    payload += static_cast<char>((size & 31) << 3);
    payload += frame.data;

    RtpPayloadUnit result;
    result.pts = frame.pts;
    result.clock_rate = sample_rate;
    result.payloads.push_back(std::move(payload));
    return result;
}

}  // namespace rtsp
