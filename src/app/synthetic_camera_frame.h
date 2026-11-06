#pragma once

#include "media/camera_frame.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <stdexcept>

namespace rtsp::app {

// 探测程序的可重复输入；与真实采集共用 CameraFrame/编码路径，不枚举或打开设备。
inline CameraFrame synthetic_camera_frame(int width, int height, const std::string& format,
                                          int fps_num, int fps_den, std::int64_t index) {
    AVFrame* raw = av_frame_alloc();
    if (!raw) { throw std::bad_alloc(); }
    std::shared_ptr<AVFrame> owned(raw, [](AVFrame* frame) { av_frame_free(&frame); });
    raw->width = width;
    raw->height = height;
    raw->format = format == "yuyv422" ? AV_PIX_FMT_YUYV422 : AV_PIX_FMT_BGR24;
    if (av_frame_get_buffer(raw, 64) < 0) { throw std::runtime_error("allocate synthetic frame failed"); }
    for (int y = 0; y < height; ++y) {
        auto* row = raw->data[0] + y * raw->linesize[0];
        for (int x = 0; x < width; ++x) {
            const auto value = static_cast<std::uint8_t>(32 + (x + y + index * 3) % 180);
            if (format == "yuyv422") {
                row[x * 2] = value;
                row[x * 2 + 1] = 128;
            } else {
                row[x * 3] = value;
                row[x * 3 + 1] = static_cast<std::uint8_t>(32 + (y + index * 2) % 180);
                row[x * 3 + 2] = static_cast<std::uint8_t>(32 + (x + index) % 180);
            }
        }
    }
    CameraFrame result;
    result.buffer = std::move(owned);
    result.width = width;
    result.height = height;
    result.pixel_format = format;
    result.sequence = static_cast<std::uint64_t>(index);
    std::copy_n(raw->linesize, result.linesize.size(), result.linesize.begin());
    result.source_pts = index;
    result.time_base_num = fps_den;
    result.time_base_den = fps_num;
    result.source_duration = 1;
    return result;
}

}
