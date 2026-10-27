#pragma once

#include "media/aac_access_unit.h"
#include "rtp/rtp_payload.h"

namespace rtsp {

class AacPacketizer {
public:
    RtpPayloadUnit packetize(const AacAccessUnit& frame, int sample_rate) const;
};

}  // namespace rtsp
