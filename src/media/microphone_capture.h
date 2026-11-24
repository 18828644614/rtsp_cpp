#pragma once

#include "media/audio_frame.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace rtsp {

struct MicrophoneDeviceInfo {
    std::string name;         // 用于打开的 DirectShow 标识。
    std::string description;  // 用于显示的友好名称。
};

struct MicrophoneConfig {
    std::string device_name;
    int sample_rate = 0;      // 0：使用设备默认值。
    int channels = 0;
    int audio_buffer_ms = 50;
};

struct MicrophoneFormat {
    int sample_rate = 0;
    int channels = 0;
    std::string sample_format;
    std::string channel_layout;
    std::string codec;
    int time_base_num = 0;
    int time_base_den = 1;
};

enum class MicrophoneReadResult {
    FrameReady,
    Timeout,
    Stopped,
    Failed
};

// open/read/close/查询由同一线程调用；仅 request_stop 允许从其他线程调用。
class MicrophoneCapture {
public:
    MicrophoneCapture();
    ~MicrophoneCapture();
    MicrophoneCapture(const MicrophoneCapture&) = delete;
    MicrophoneCapture& operator=(const MicrophoneCapture&) = delete;

    static std::string ffmpeg_version();
    static std::vector<MicrophoneDeviceInfo> enumerate_devices();

    // 外部停止标记须存活到 close；打开设备期间的停止请求也不会被覆盖。
    void open(const MicrophoneConfig& config, const std::atomic<bool>* stop_requested = nullptr);
    // 0 表示非阻塞尝试；负超时抛异常，非 FrameReady 时清空 frame。
    MicrophoneReadResult read(AudioFrame& frame, std::chrono::milliseconds timeout);
    void request_stop();
    void close();

    const MicrophoneFormat& format() const;
    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rtsp
