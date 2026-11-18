#include "media/microphone_capture_input.h"

#include <stdexcept>

namespace rtsp {
namespace {

class CaptureInput final : public AudioFrameInput {
public:
    explicit CaptureInput(MicrophoneConfig config) : config_(std::move(config)) {}

    AudioInputResult read(AudioFrame& frame, std::chrono::milliseconds timeout) override {
        if (stopping_.load()) {
            return AudioInputResult::Ended;
        }
        if (!opened_) {
            capture_.open(config_, &stopping_);
            opened_ = true;
        }
        switch (capture_.read(frame, timeout)) {
        case MicrophoneReadResult::FrameReady: return AudioInputResult::FrameReady;
        case MicrophoneReadResult::Timeout: return AudioInputResult::Waiting;
        case MicrophoneReadResult::Stopped: return AudioInputResult::Ended;
        case MicrophoneReadResult::Failed: throw std::runtime_error(capture_.last_error());
        }
        throw std::logic_error("unknown microphone read result");
    }

    void request_stop() noexcept override {
        stopping_.store(true);
        capture_.request_stop();
    }
    void close() noexcept override {
        capture_.close();
    }

private:
    const MicrophoneConfig config_;
    std::atomic<bool> stopping_{false};
    MicrophoneCapture capture_;
    bool opened_ = false;
};

}

std::unique_ptr<AudioFrameInput> make_microphone_capture_input(const MicrophoneConfig& config) {
    return std::make_unique<CaptureInput>(config);
}

}
