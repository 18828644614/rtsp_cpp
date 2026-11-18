#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct AVFrame;

namespace rtsp {

// 原始 PCM。完整声道布局和各平面的内存布局保存在 buffer 中。
struct AudioFrame {
    std::shared_ptr<const AVFrame> buffer;
    std::uint64_t sequence = 0;
    int sample_rate = 0;
    int channels = 0;
    std::string sample_format;
    int nb_samples = 0;  // 每个声道的采样数，不是字节数。

    // 保留设备时间，后续由共同时间线映射；到达时间不等同于采样时间。
    // 与下面的 time_base 配套使用，不与其他设备的原始 PTS 直接比较。
    std::optional<std::int64_t> source_pts;
    int time_base_num = 0;
    int time_base_den = 1;
    std::int64_t arrival_steady_us = 0;
};

}  // namespace rtsp
