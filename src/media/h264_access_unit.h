#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rtsp {

struct H264AccessUnit {
    std::vector<std::string> nalus;
    bool is_keyframe = false;
    // 时基为 90000 Hz；文件源按解码顺序生成固定 25 fps 时间，编码源按其输入时间生成。
    // nalus 包含完整 NAL 头和负载，不包含 Annex-B 起始码；is_keyframe 表示包含 IDR。
    std::uint64_t pts = 0;
    std::uint32_t duration = 0;
};

}  // namespace rtsp
