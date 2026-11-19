#include "support/synthetic_camera_frame.h"
#include "media/camera_video_source.h"
#include "media/camera_capture_input.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void check(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
template<class Predicate> void await(Predicate done) {
    const auto until = Clock::now() + 3s;
    while (!done()) {
        check(Clock::now() < until, "timed out waiting for producer");
        std::this_thread::sleep_for(1ms);
    }
}
struct InputState {
    std::mutex mutex;
    std::condition_variable wake;
    int credits = 1;
    bool eof = false;
    bool fail = false;
    bool stopped = false;
    bool closed = false;
    std::thread::id reader;
    void update(int more = 0, bool end = false, bool error = false) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            credits += more;
            eof = end;
            fail = error;
        }
        wake.notify_all();
    }
};
rtsp::CameraVideoSourceConfig config() {
    rtsp::CameraVideoSourceConfig result;
    result.timeline = rtsp::CaptureTimeline(Clock::time_point{});
    result.encoder.width = 64;
    result.encoder.height = 48;
    result.encoder.gop = 3;
    result.startup_timeout = 2s;
    result.read_timeout = 20ms;
    return result;
}
class GeneratedInput final : public rtsp::CameraFrameInput {
public:
    explicit GeneratedInput(std::shared_ptr<InputState> state, std::int64_t first_arrival_us = 0)
        : state_(std::move(state)), first_arrival_us_(first_arrival_us) {}
    rtsp::CameraInputResult read(rtsp::CameraFrame& frame, std::chrono::milliseconds timeout) override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->reader = std::this_thread::get_id();
        state_->wake.wait_for(lock, timeout, [&] {
            return state_->stopped || state_->fail || state_->eof || state_->credits > 0;
        });
        if (state_->stopped) {
            return rtsp::CameraInputResult::Ended;
        }
        if (state_->fail) {
            throw std::runtime_error("simulated device lost");
        }
        if (state_->credits == 0) {
            return state_->eof ? rtsp::CameraInputResult::Ended : rtsp::CameraInputResult::Waiting;
        }
        --state_->credits;
        lock.unlock();
        frame = rtsp::test::synthetic_camera_frame(64, 48, "yuyv422", 30, 1, index_++);
        frame.arrival_steady_us = first_arrival_us_ + *frame.source_pts * 50000;
        return rtsp::CameraInputResult::FrameReady;
    }
    void request_stop() noexcept override {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->stopped = true;
        }
        state_->wake.notify_all();
    }
    void close() noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closed = true;
    }
private:
    std::shared_ptr<InputState> state_;
    const std::int64_t first_arrival_us_;
    std::int64_t index_ = 0;
};
std::unique_ptr<rtsp::CameraVideoSource> source(const std::shared_ptr<InputState>& state,
                                              rtsp::CameraVideoSourceConfig cfg = config()) {
    return std::make_unique<rtsp::CameraVideoSource>(std::make_unique<GeneratedInput>(state), cfg);
}
void check_released(const std::shared_ptr<InputState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    check(state->closed && state->stopped, "input not stopped and closed");
    check(state->reader != std::this_thread::get_id(), "capture ran on consumer thread");
}
void test_source_wait_drain_stop() {
    const auto state = std::make_shared<InputState>();
    auto camera = source(state);
    const auto fmtp = camera->track().fmtp;
    check(fmtp.find("sprop-parameter-sets=") != std::string::npos, "startup did not publish SPS/PPS");
    rtsp::H264AccessUnit frame;
    check(camera->read(frame) && frame.is_keyframe && frame.pts == 0 && frame.duration == 3000,
          "startup did not retain complete IDR/time");
    check(!camera->read(frame) && frame.nalus.empty() && !camera->ended(), "waiting confused with EOF");
    state->update(2, true);
    await([&] {
        return camera->queue_stats().closed;
    });
    check(!camera->ended(), "producer marked EOF before consumer drained");
    check(camera->read(frame) && frame.pts == 3000 && !frame.is_keyframe, "first P/time missing");
    check(camera->read(frame) && frame.pts == 6000, "EOF lost final encoded frame");
    check(!camera->read(frame) && camera->ended(), "EOF not latched after empty read");
    check(camera->track().fmtp == fmtp, "track changed concurrently");
    camera->stop();
    camera->stop();
    check_released(state);

    const auto blocked = std::make_shared<InputState>();
    auto waiting = source(blocked);
    const auto start = Clock::now();
    waiting.reset();
    check(Clock::now() - start < 1s, "destruction did not interrupt blocked input");
    check_released(blocked);
}
void test_capture_timeline() {
    const Clock::time_point epoch{1s};
    auto cfg = config();
    cfg.timeline = rtsp::CaptureTimeline(epoch);
    for (const int start_ms : {80, 200}) {
        auto state = std::make_shared<InputState>();
        rtsp::CameraVideoSource camera(
            std::make_unique<GeneratedInput>(state, 1000000 + start_ms * 1000), cfg);
        const auto pts = start_ms * 90;
        rtsp::H264AccessUnit frame;
        check(camera.read(frame) && frame.is_keyframe && frame.pts == pts,
              "H264 encoder erased the capture offset");
        state->update(2, true);
        await([&] {
            return camera.queue_stats().closed;
        });
        check(camera.read(frame) && frame.pts == pts + 3000 && camera.read(frame) && frame.pts == pts + 6000,
              "video used arrival jitter instead of device PTS increments");
        check(!camera.read(frame) && camera.ended(), "timeline source did not drain");
    }
}

void test_startup_and_runtime_failures() {
    for (int mode = 0; mode < 4; ++mode) {
        const auto state = std::make_shared<InputState>();
        auto cfg = config();
        if (mode != 3) {
            state->credits = 0;
        }
        if (mode == 0) {
            cfg.startup_timeout = 50ms;
        }
        if (mode == 1) {
            state->eof = true;
        }
        if (mode == 2) {
            state->fail = true;
        }
        if (mode == 3) {
            cfg.queue.max_bytes = 1;
        }
        bool failed = false;
        try {
            auto unused = source(state, cfg);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        check(failed, "startup failure was not reported");
        check_released(state);
    }
    const auto state = std::make_shared<InputState>();
    auto camera = source(state);
    state->update(0, false, true);
    await([&] {
        return camera->queue_stats().closed;
    });
    rtsp::H264AccessUnit frame;
    check(camera->read(frame), "runtime error lost buffered IDR");
    bool failed = false;
    try {
        camera->read(frame);
    } catch (const std::runtime_error& e) {
        failed = std::string(e.what()) == "simulated device lost";
    }
    check(failed, "runtime error not delivered to consumer");
}
void test_overflow_and_resume() {
    const auto state = std::make_shared<InputState>();
    auto cfg = config();
    cfg.queue.max_frames = 2;
    auto camera = source(state, cfg);
    state->update(4); // 0/1 入队，2 溢出丢弃；3(IDR)/4 保留。
    await([&] {
        return camera->queue_stats().dropped == 3 && camera->queue_stats().frames == 2;
    });
    rtsp::H264AccessUnit frame;
    check(camera->read(frame) && frame.is_keyframe && frame.pts == 9000, "overflow did not recover on IDR");
    camera->resume(); // 剩下的 4 是 P，必须丢弃。
    check(!camera->read(frame) && !camera->ended(), "resume retained undecodable P");
    state->update(2); // 5 丢弃，6(IDR) 恢复。
    await([&] {
        return camera->queue_stats().frames == 1;
    });
    check(camera->read(frame) && frame.is_keyframe && frame.pts == 18000, "resume missed next IDR");
}

void test_startup_cancellation() {
    auto state = std::make_shared<InputState>();
    state->credits = 0;
    std::atomic<bool> stopped{false};
    std::exception_ptr failure;
    std::thread constructor([&] {
        try {
            rtsp::CameraVideoSource camera(std::make_unique<GeneratedInput>(state), config(), &stopped);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    // 不在生产线程停止前读取未加锁的 reader/closed。
    std::this_thread::sleep_for(30ms);
    stopped.store(true);
    constructor.join();
    bool cancelled = false;
    try {
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const std::runtime_error& error) {
        cancelled = std::string(error.what()) == "camera startup stopped";
    }
    check(cancelled, "startup did not observe console stop");
    check_released(state);
}

void test_capture_adapter_without_device() {
    auto input = rtsp::make_camera_capture_input({});
    input->request_stop();
    bool stopped = false;
    try {
        rtsp::CameraVideoSource camera(std::move(input), config());
    } catch (const std::runtime_error&) {
        stopped = true;
    }
    check(stopped, "pre-stopped capture adapter opened device or accepted startup");

    rtsp::CameraConfig invalid;
    invalid.width = 0;
    bool rejected = false;
    try {
        rtsp::CameraVideoSource camera(rtsp::make_camera_capture_input(invalid), config());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "capture adapter did not propagate worker open failure");
}
}

int main() {
    try {
        test_source_wait_drain_stop();
        test_capture_timeline();
        test_startup_and_runtime_failures();
        test_overflow_and_resume();
        test_startup_cancellation();
        test_capture_adapter_without_device();
        std::cout << "PASS: generated capture/encode, live wait/end/failure, overflow, resume, RTP decode and stop\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
