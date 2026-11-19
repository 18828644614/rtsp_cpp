#include "support/synthetic_audio_input.h"

#include <iostream>
#include <optional>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void check(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    bool ended = false;
    bool failed = false;
    bool stopped = false;
    std::atomic<bool> closed{false};
    std::atomic<bool> same_thread{false};
    void finish(bool failure = false) {
        std::lock_guard<std::mutex> lock(mutex);
        ended = !failure;
        failed = failure;
        wake.notify_all();
    }
};

class ControlledInput final : public rtsp::AudioFrameInput {
public:
    ControlledInput(std::shared_ptr<State> state, bool pcm = true,
                    std::optional<std::int64_t> first_arrival_us = std::nullopt)
        : state_(std::move(state)), pcm_(pcm), first_arrival_us_(first_arrival_us), input_(3000, 48000, 1) {}

    rtsp::AudioInputResult read(rtsp::AudioFrame& frame, std::chrono::milliseconds timeout) override {
        owner_ = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (pcm_ && !state_->stopped && !state_->failed) {
            if (input_.read(frame, 0ms) == rtsp::AudioInputResult::FrameReady) {
                if (first_arrival_us_) {
                    frame.arrival_steady_us = *first_arrival_us_ + *frame.source_pts * 30;
                }
                return rtsp::AudioInputResult::FrameReady;
            }
        }
        state_->wake.wait_for(lock, timeout, [&] {
            return state_->stopped || state_->ended || state_->failed;
        });
        if (state_->failed) {
            throw std::runtime_error("simulated microphone failure");
        }
        return state_->stopped || state_->ended ? rtsp::AudioInputResult::Ended : rtsp::AudioInputResult::Waiting;
    }

    void request_stop() noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopped = true;
        state_->wake.notify_all();
    }

    void close() noexcept override {
        state_->same_thread.store(owner_ == std::this_thread::get_id());
        state_->closed.store(true);
    }

private:
    std::shared_ptr<State> state_;
    const bool pcm_;
    const std::optional<std::int64_t> first_arrival_us_;
    rtsp::test::SyntheticAudioInput input_;
    std::thread::id owner_;
};

void lifecycle() {
    for (const bool failure : {false, true}) {
        auto state = std::make_shared<State>();
        rtsp::MicrophoneAudioSource source(std::make_unique<ControlledInput>(state));
        check(source.track().clock_rate == 48000, "audio startup did not publish track");
        state->finish(failure);
        int frames = 0;
        bool raised = false;
        std::optional<std::int64_t> previous;
        const auto deadline = Clock::now() + 2s;
        try {
            while (!source.ended()) {
                check(Clock::now() < deadline, "audio source failed to reach EOF");
                rtsp::AacAccessUnit unit;
                if (source.read(unit)) {
                    check(!previous || unit.pts == *previous + 1024, "audio source lost or reordered packets");
                    previous = unit.pts;
                    ++frames;
                } else {
                    std::this_thread::yield();
                }
            }
        } catch (const std::runtime_error& error) {
            check(std::string(error.what()) == "simulated microphone failure", "unexpected source error");
            raised = true;
        }
        check(raised == failure && frames > 0, "source did not drain data before EOF/error");
        if (!failure) {
            check(frames == 4, "normal EOF lost FIFO tail or delayed AAC");
        }
        check(source.queue_stats().dropped == 0, "test queue unexpectedly overflowed");
        source.stop();
        source.stop();
        check(state->closed.load() && state->same_thread.load() && source.ended(), "stop leaked worker/input");
    }

    for (const bool cancellation : {false, true}) {
        auto state = std::make_shared<State>();
        rtsp::MicrophoneAudioSourceConfig config;
        config.startup_timeout = 40ms;
        config.read_timeout = 10ms;
        std::atomic<bool> stopped{cancellation};
        bool raised = false;
        try {
            rtsp::MicrophoneAudioSource source(std::make_unique<ControlledInput>(state, false), config, &stopped);
        } catch (const std::runtime_error&) {
            raised = true;
        }
        check(raised && state->closed.load(), "startup timeout/cancellation did not close input");
    }

    auto state = std::make_shared<State>();
    rtsp::MicrophoneAudioSourceConfig config;
    config.no_data_timeout = 50ms;
    config.read_timeout = 10ms;
    rtsp::MicrophoneAudioSource source(std::make_unique<ControlledInput>(state), config);
    bool raised = false;
    const auto deadline = Clock::now() + 1s;
    while (Clock::now() < deadline && !raised) {
        rtsp::AacAccessUnit frame;
        try {
            source.read(frame);
        } catch (const std::runtime_error&) {
            raised = true;
        }
        std::this_thread::yield();
    }
    check(raised, "PCM starvation was not reported");
}

void test_capture_timeline() {
    const Clock::time_point epoch{1s};
    for (const int start_ms : {200, 80, -40}) {
        auto state = std::make_shared<State>();
        rtsp::MicrophoneAudioSourceConfig config;
        config.timeline = rtsp::CaptureTimeline(epoch);
        // The first block has 333 samples at 48 kHz (6937 us truncated).
        rtsp::MicrophoneAudioSource source(
            std::make_unique<ControlledInput>(state, true, 1000000 + start_ms * 1000 + 6937), config);
        state->finish();
        const auto deadline = Clock::now() + 2s;
        int packets = 0;
        const auto start_pts = start_ms * 48;
        while (!source.ended()) {
            check(Clock::now() < deadline, "timeline audio did not reach EOF");
            rtsp::AacAccessUnit unit;
            if (source.read(unit)) {
                check(unit.pts == start_pts - 1024 + packets * 1024,
                      "AAC lost the configured capture offset, padding or sample continuity");
                ++packets;
            } else {
                std::this_thread::yield();
            }
        }
        check(packets == 4, "timeline audio lost tail packets");
    }
}


}

int main() {
    try {
        lifecycle();
        test_capture_timeline();
        std::cout << "PASS: startup, waiting, EOF, error draining, stop and pure audio RTP session\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
