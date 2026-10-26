#pragma once

#include "media/aac_access_unit.h"
#include "rtp/rtp_payload.h"

#include <memory>

namespace rtsp {

class AacPacketizer {
public:
    std::shared_ptr<const RtpPayloadUnit> packetize(const AacAccessUnit& frame,
                                                  int sample_rate) const;
};

}  // namespace rtsp
