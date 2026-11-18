#pragma once

#include "media/audio_ffmpeg.h"
#include "media/microphone_audio_source.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>

namespace rtsp::app {

class SyntheticAudioInput final : public AudioFrameInput {
public:
    SyntheticAudioInput(std::int64_t samples = 0, int rate = 44100, int channels = 2,
                        bool realtime = false)
        : total_(samples), rate_(rate), channels_(channels), realtime_(realtime) {
        if (samples < 0 || rate <= 0 || channels < 1 || channels > 2) {
            throw std::invalid_argument("invalid synthetic audio configuration");
        }
    }

    AudioInputResult read(AudioFrame& frame, std::chrono::milliseconds timeout) override {
        frame = {};
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_ || (total_ && position_ >= total_)) {
            return AudioInputResult::Ended;
        }
        if (realtime_) {
            if (!started_) {
                origin_ = std::chrono::steady_clock::now();
                started_ = true;
            }
            const auto due = origin_ + std::chrono::microseconds(position_ * 1000000 / rate_);
            wake_.wait_until(lock, std::min(due, std::chrono::steady_clock::now() + timeout),
                             [this] {
                                 return stopped_;
                             });
            if (stopped_) {
                return AudioInputResult::Ended;
            }
            if (std::chrono::steady_clock::now() < due) {
                return AudioInputResult::Waiting;
            }
        }
        const int count = static_cast<int>(std::min<std::int64_t>(333,
            total_ ? total_ - position_ : 333));
        auto buffer = detail::audio_buffer(rate_, channels_, AV_SAMPLE_FMT_FLT, count);
        auto* samples = reinterpret_cast<float*>(buffer->data[0]);
        for (int index = 0; index < count; ++index) {
            const auto position = position_ + index;
            const float value = position % rate_ == rate_ / 4 ? 0.8f :
                0.04f * static_cast<float>(std::sin(6.283185307179586 * 440 * position / rate_));
            for (int channel = 0; channel < channels_; ++channel) {
                samples[index * channels_ + channel] = value;
            }
        }
        frame.buffer = buffer;
        frame.sample_rate = rate_;
        frame.channels = channels_;
        frame.sample_format = "flt";
        frame.nb_samples = count;
        frame.source_pts = position_;
        frame.time_base_num = 1;
        frame.time_base_den = rate_;
        frame.arrival_steady_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        position_ += count;
        return AudioInputResult::FrameReady;
    }

    void request_stop() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        wake_.notify_all();
    }

private:
    const std::int64_t total_;
    const int rate_;
    const int channels_;
    const bool realtime_;
    std::int64_t position_ = 0;
    bool stopped_ = false;
    bool started_ = false;
    std::chrono::steady_clock::time_point origin_{};
    std::mutex mutex_;
    std::condition_variable wake_;
};

}
