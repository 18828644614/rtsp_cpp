#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rtsp {

struct H264AccessUnit {
    std::vector<std::string> nalus;
    bool is_keyframe = false;
    // 当前按解码顺序生成简化时间值，时基为 90000 Hz，不解析真实 PTS/DTS。
    std::uint64_t pts = 0;
    std::uint32_t duration = 0;
};

}  // namespace rtsp
