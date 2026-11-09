#pragma once

#include "media/camera_frame.h"

#include <chrono>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace rtsp {

struct CameraDeviceInfo {
    std::string name;
    std::string description;
};

struct CameraConfig {
    std::string device_name = "Integrated Camera";
    int width = 1280;
    int height = 720;
    int fps_num = 30;
    int fps_den = 1;
    std::string pixel_format = "yuyv422";
};

struct CameraFormat {
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 1;
    std::string pixel_format;
    std::string codec;
    int time_base_num = 0;
    int time_base_den = 1;
};

enum class CaptureResult { FrameReady, Timeout, Stopped, Failed };

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();
    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    static std::string ffmpeg_version();
    static std::vector<CameraDeviceInfo> enumerate_devices();

    // 可选外部停止标记须存活到 close；用于工作线程打开设备时保留并发停止请求。
    void open(const CameraConfig& config, const std::atomic<bool>* stop_requested = nullptr);
    CaptureResult read(CameraFrame& frame, std::chrono::milliseconds timeout);
    void request_stop();
    void close();

    const CameraFormat& format() const;
    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
