#pragma once

#include "media/aac_encoder.h"
#include "media/audio_frame_queue.h"
#include "media/audio_source.h"
#include "media/capture_timeline.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace rtsp {

enum class AudioInputResult {
    FrameReady,
    Waiting,
    Ended
};

class AudioFrameInput {
public:
    virtual ~AudioFrameInput() = default;
    virtual AudioInputResult read(AudioFrame& frame, std::chrono::milliseconds timeout) = 0;
    virtual void request_stop() noexcept = 0;
    virtual void close() noexcept {}
};

struct MicrophoneAudioSourceConfig {
    AacEncoderConfig encoder;
    AudioFrameQueueLimits queue{9};  // 9 个 AAC AU 在 48 kHz 下约 192 ms。
    std::chrono::milliseconds startup_timeout{5000};
    std::chrono::milliseconds read_timeout{100};
    std::chrono::milliseconds no_data_timeout{5000};
    CaptureTimeline timeline;
};

class MicrophoneAudioSource final : public AudioSource {
public:
    MicrophoneAudioSource(std::unique_ptr<AudioFrameInput> input,
                          MicrophoneAudioSourceConfig config = {},
                          const std::atomic<bool>* stop_requested = nullptr);
    ~MicrophoneAudioSource() override;

    bool read(AacAccessUnit& frame) override;
    bool ended() const override;
    const MediaTrack& track() const override;
    void check_health() const override;
    void stop() override;
    AudioFrameQueueStats queue_stats() const;

private:
    void produce() noexcept;
    void publish(std::vector<AacAccessUnit> frames, const AacEncoder& encoder, std::int64_t start_pts);
    std::unique_ptr<AudioFrameInput> input_;
    const MicrophoneAudioSourceConfig config_;
    AudioFrameQueue queue_;
    MediaTrack track_;
    std::atomic<bool> stopping_{false};
    bool ended_ = false;
    mutable std::mutex startup_mutex_;
    std::condition_variable startup_wake_;
    bool ready_ = false;
    bool producer_done_ = false;
    std::exception_ptr startup_error_;
    std::thread worker_;
};

}
