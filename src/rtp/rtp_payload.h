#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rtsp {

// 一帧媒体生成的一组 RTP 负载，不含 RTP 头和 TCP 头。
// 所有包共用一个时间戳，只有最后一包设置 Marker。
struct RtpPayloadUnit {
    std::uint64_t pts = 0;
    int clock_rate = 90000;
    bool is_keyframe = false;
    std::vector<std::string> payloads;
};

}  // namespace rtsp
