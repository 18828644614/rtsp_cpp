#pragma once

#include <cstdint>
#include <string>

namespace rtsp {

struct AacAccessUnit {
    std::string data;                  // 裸 AAC，不含 ADTS 头。
    std::uint64_t pts = 0;             // 时基为音频采样率。
    std::uint32_t duration = 1024;      // 第一版仅支持每帧 1024 采样的 AAC-LC。
};

}  // namespace rtsp
