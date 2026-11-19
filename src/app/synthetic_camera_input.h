#pragma once

#include "app/synthetic_camera_frame.h"
#include "media/camera_video_source.h"

namespace rtsp::app {

class SyntheticCameraInput final : public CameraFrameInput {
public:
    explicit SyntheticCameraInput(H264EncoderConfig config) : config_(std::move(config)) {}

    CameraInputResult read(CameraFrame& frame, std::chrono::milliseconds timeout) override {
        using namespace std::chrono;
        std::unique_lock<std::mutex> lock(mutex_);
        const auto now = steady_clock::now();
        if (index_ == 0) {
            origin_ = now;
        }
        const auto due = origin_ + microseconds(index_ * 1000000 / config_.fps_num);
        wake_.wait_until(lock, std::min(due, now + timeout), [this] { return stopped_; });
        if (stopped_) {
            return CameraInputResult::Ended;
        }
        if (steady_clock::now() < due) {
            return CameraInputResult::Waiting;
        }
        lock.unlock();
        frame = synthetic_camera_frame(config_.width, config_.height, config_.pixel_format,
                                       config_.fps_num, 1, index_++);
        frame.arrival_steady_us = duration_cast<microseconds>(due.time_since_epoch()).count();
        return CameraInputResult::FrameReady;
    }

    void request_stop() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        wake_.notify_all();
    }

private:
    const H264EncoderConfig config_;
    std::chrono::steady_clock::time_point origin_{};
    std::int64_t index_ = 0;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopped_ = false;
};

}  // namespace rtsp::app
