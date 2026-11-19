#include "media/microphone_audio_source.h"
#include "media/media_time.h"

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
            if (stop_requested && stop_requested->load()) {
                throw std::runtime_error("audio startup stopped");
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                throw std::runtime_error("audio startup timed out waiting for AAC");
            }
            startup_wake_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)));
        }
        if (!ready_) {
            if (startup_error_) {
                std::rethrow_exception(startup_error_);
            }
            throw std::runtime_error("audio ended before AAC became available");
        }
    } catch (...) {
        stop();
        throw;
    }
}

MicrophoneAudioSource::~MicrophoneAudioSource() {
    stop();
}

void MicrophoneAudioSource::publish(std::vector<AacAccessUnit> frames, const AacEncoder& encoder,
                                    std::int64_t start_pts) {
    for (auto& frame : frames) {
        if (stopping_.load()) {
            return;
        }
        frame.pts = add_media_pts(frame.pts, start_pts);
        if (!queue_.push(std::move(frame))) {
            return;
        }
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
        std::optional<std::int64_t> start_pts;
        auto last_data = std::chrono::steady_clock::now();
        while (!stopping_.load()) {
            AudioFrame frame;
            const auto result = input_->read(frame, config_.read_timeout);
            if (stopping_.load()) {
                break;
            }
            if (result == AudioInputResult::Waiting) {
                if (std::chrono::steady_clock::now() - last_data >= config_.no_data_timeout) {
                    throw std::runtime_error("audio input timed out without PCM");
                }
                continue;
            }
            if (result == AudioInputResult::Ended) {
                publish(encoder.flush(), encoder, start_pts.value_or(0));
                break;
            }
            last_data = std::chrono::steady_clock::now();
            if (config_.timeline && !start_pts) {
                // 用首块结束附近的到达时刻估算首个采样时刻；不逐块重打 PTS。
                const auto block_us = rescale_media_pts(frame.nb_samples, frame.sample_rate, 1000000);
                const auto first_us = add_media_pts(
                    config_.timeline->relative_us(frame.arrival_steady_us), -block_us);
                start_pts = rescale_media_pts(first_us, 1000000, encoder.track().clock_rate);
            }
            publish(encoder.encode(frame), encoder, start_pts.value_or(0));
        }
    } catch (...) {
        error = std::current_exception();
    }
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
    if (result == AudioFrameRead::Ended) {
        ended_ = true;
    }
    return result == AudioFrameRead::FrameReady;
}

bool MicrophoneAudioSource::ended() const {
    return ended_;
}
const MediaTrack& MicrophoneAudioSource::track() const {
    return track_;
}
void MicrophoneAudioSource::resume() {
    queue_.resume_at_latest();
}
AudioFrameQueueStats MicrophoneAudioSource::queue_stats() const {
    return queue_.stats();
}

void MicrophoneAudioSource::stop() {
    stopping_.store(true);
    input_->request_stop();
    if (worker_.joinable()) {
        worker_.join();
    }
    queue_.discard_and_finish();
    ended_ = true;
}

}
