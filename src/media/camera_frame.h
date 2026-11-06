#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct AVFrame;

namespace rtsp {

struct CameraFrame {
    // 原始图像及其内存布局
    std::shared_ptr<const AVFrame> buffer;
    std::uint64_t sequence = 0;
    // 外层记录的图像属性
    int width = 0;
    int height = 0;
    std::string pixel_format;

    std::array<int, 4> linesize{};
    // 原始图像的时间信息
    std::optional<std::int64_t> source_pts;
    int time_base_num = 0;
    int time_base_den = 1;
    std::int64_t source_duration = 0;
    std::optional<std::int64_t> relative_pts_us;
    std::int64_t arrival_steady_us = 0;
};

}
