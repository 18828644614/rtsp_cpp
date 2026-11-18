#include "media/microphone_audio_source.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rtsp {

MicrophoneAudioSource::MicrophoneAudioSource(std::unique_ptr<AudioFrameInput> input,
    MicrophoneAudioSourceConfig config, const std::atomic<bool>* stop_requested)
    : input_(std::move(input)), config_(config), queue_(config.queue) {
    if (!input_ || config.startup_timeout.count() <= 0 || config.read_timeout.count() <= 0 ||
        config.no_data_timeout.count() <= 0) {
        throw std::invalid_argument("audio source requires input and positive timeouts");
    }
    worker_ = std::thread(&MicrophoneAudioSource::produce, this);
    try {
        std::unique_lock<std::mutex> lock(startup_mutex_);
        const auto deadline = std::chrono::steady_clock::now() + config.startup_timeout;
        while (!ready_ && !producer_done_) {
            if (stop_requested && stop_requested->load()) { throw std::runtime_error("audio startup stopped"); }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) { throw std::runtime_error("audio startup timed out waiting for AAC"); }
            startup_wake_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)));
        }
        if (!ready_) {
            if (startup_error_) { std::rethrow_exception(startup_error_); }
            throw std::runtime_error("audio ended before AAC became available");
        }
    } catch (...) {
        stop();
        throw;
    }
}

MicrophoneAudioSource::~MicrophoneAudioSource() { stop(); }

void MicrophoneAudioSource::publish(std::vector<AacAccessUnit> frames, const AacEncoder& encoder) {
    for (auto& frame : frames) {
        if (stopping_.load()) { return; }
        if (!queue_.push(std::move(frame))) { return; }
        if (!ready_) {
            std::lock_guard<std::mutex> lock(startup_mutex_);
            track_ = encoder.track();
            ready_ = true;
            startup_wake_.notify_all();
        }
    }
}

void MicrophoneAudioSource::produce() noexcept {
    std::exception_ptr error;
    try {
        AacEncoder encoder;
        encoder.open(config_.encoder);
        auto last_data = std::chrono::steady_clock::now();
        while (!stopping_.load()) {
            AudioFrame frame;
            const auto result = input_->read(frame, config_.read_timeout);
            if (stopping_.load()) { break; }
            if (result == AudioInputResult::Waiting) {
                if (std::chrono::steady_clock::now() - last_data >= config_.no_data_timeout) {
                    throw std::runtime_error("audio input timed out without PCM");
                }
                continue;
            }
            if (result == AudioInputResult::Ended) { publish(encoder.flush(), encoder); break; }
            last_data = std::chrono::steady_clock::now();
            publish(encoder.encode(frame), encoder);
        }
    } catch (...) { error = std::current_exception(); }
    input_->close();
    queue_.finish(error);
    {
        std::lock_guard<std::mutex> lock(startup_mutex_);
        startup_error_ = error;
        producer_done_ = true;
    }
    startup_wake_.notify_all();
}

bool MicrophoneAudioSource::read(AacAccessUnit& frame) {
    const auto result = queue_.read(frame);
    if (result == AudioFrameRead::Ended) { ended_ = true; }
    return result == AudioFrameRead::FrameReady;
}

bool MicrophoneAudioSource::ended() const { return ended_; }
const MediaTrack& MicrophoneAudioSource::track() const { return track_; }
void MicrophoneAudioSource::resume() { queue_.resume_at_latest(); }
AudioFrameQueueStats MicrophoneAudioSource::queue_stats() const { return queue_.stats(); }

void MicrophoneAudioSource::stop() {
    stopping_.store(true);
    input_->request_stop();
    if (worker_.joinable()) { worker_.join(); }
    queue_.discard_and_finish();
    ended_ = true;
}

}
