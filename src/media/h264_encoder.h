#pragma once

#include "media/camera_frame.h"
#include "media/h264_access_unit.h"

#include <memory>
#include <string>
#include <vector>

namespace rtsp {

struct H264EncoderConfig {
    int width = 1280;
    int height = 720;
    int fps_num = 30;
    int fps_den = 1;
    // 目标码率，默认 2 Mbit/s
    int bitrate = 2000000;
    // 关键帧间隔，默认 30 帧
    int gop = 30;
    std::string pixel_format = "yuyv422";
};

enum class EncoderTimestampMode { Unset, Source, FrameRate };

// 单个所有者线程顺序调用；输入帧只读，输出拥有独立的 NALU 字节。
// 首版颜色约定：limited-range BT.601 YUYV 或 full-range BGR，输出 limited BT.601。
class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // 根据配置准备好编码器、像素转换器和缓冲区，让后续 encode() 可以直接处理图像
    void open(const H264EncoderConfig& config);

    // 接收一幅原始图像，转换为 YUV420P，提交给 x264 编码，并取出当前可获得的 H.264 访问单元
    std::vector<H264AccessUnit> encode(const CameraFrame& frame);
    // 排空后不能继续输入；重复 flush 返回空集合。close 不隐式排空。
    std::vector<H264AccessUnit> flush();
    void close() noexcept;

    // 首个编码输出前可能为空；均为带 NAL 头、不带起始码的字节。
    const std::string& sps() const;
    const std::string& pps() const;
    EncoderTimestampMode timestamp_mode() const;
    std::uint64_t timestamp_fallbacks() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
