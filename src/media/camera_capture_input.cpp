#include "media/camera_capture_input.h"

#include <stdexcept>

namespace rtsp {
namespace {
class CaptureInput final : public CameraFrameInput {
public:
    explicit CaptureInput(const CameraConfig& config) : config_(config) {}
    CameraInputResult read(CameraFrame& frame, std::chrono::milliseconds timeout) override {
        if (stopping_.load()) { return CameraInputResult::Ended; }
        if (!opened_) {
            // DirectShow 的 COM 创建与释放保持在同一个工作线程。
            capture_.open(config_, &stopping_);
            opened_ = true;
        }
        switch (capture_.read(frame, timeout)) {
        case CaptureResult::FrameReady: return CameraInputResult::FrameReady;
        case CaptureResult::Timeout: return CameraInputResult::Waiting;
        case CaptureResult::Stopped: return CameraInputResult::Ended;
        case CaptureResult::Failed: throw std::runtime_error(capture_.last_error());
        }
        throw std::logic_error("unknown camera capture result");
    }
    void request_stop() noexcept override {
        stopping_.store(true);
        capture_.request_stop();
    }
    void close() noexcept override { capture_.close(); }
private:
    const CameraConfig config_;
    std::atomic<bool> stopping_{false};
    CameraCapture capture_;
    bool opened_ = false;
};
}

std::unique_ptr<CameraFrameInput> make_camera_capture_input(const CameraConfig& config) {
    return std::make_unique<CaptureInput>(config);
}

}  // namespace rtsp
