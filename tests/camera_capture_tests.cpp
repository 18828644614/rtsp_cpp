#include "media/camera_capture.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void check(bool valid, const char* message) {
    if (!valid) { throw std::runtime_error(message); }
}

template <typename Action>
void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "invalid camera configuration was accepted");
}

void configuration_tests() {
    rtsp::CameraCapture capture;
    rtsp::CameraConfig config;
    config.width = 0;
    rejects([&] { capture.open(config); });
    config = {};
    config.height = -1;
    rejects([&] { capture.open(config); });
    config = {};
    config.fps_den = 0;
    rejects([&] { capture.open(config); });
    config = {};
    config.device_name.clear();
    rejects([&] { capture.open(config); });
    config = {};
    config.pixel_format = "mjpeg";
    rejects([&] { capture.open(config); });
    rtsp::CameraFrame frame;
    frame.width = 123;
    frame.source_pts = 456;
    check(capture.read(frame, std::chrono::milliseconds(1)) == rtsp::CaptureResult::Failed,
        "closed capture must fail");
    check(frame.width == 0 && !frame.source_pts && !frame.buffer, "failed read returned stale frame");
    check(!capture.last_error().empty(), "failed read needs an error message");
    capture.request_stop();
    check(capture.read(frame, std::chrono::milliseconds(1)) == rtsp::CaptureResult::Stopped,
        "stop request must be observable");
    capture.close();
    capture.close();
    check(capture.format().width == 0, "closed format must be cleared");
    std::atomic<bool> cancelled{true};
    bool rejected_cancelled_open = false;
    try { capture.open(rtsp::CameraConfig{}, &cancelled); }
    catch (const std::runtime_error& error) {
        rejected_cancelled_open = std::string(error.what()) == "camera open cancelled";
    }
    check(rejected_cancelled_open, "opening reset an external stop request");
    capture.close();
}

void hardware_tests(const std::string& device) {
    rtsp::CameraConfig config;
    config.device_name = device;
    rtsp::CameraCapture capture;
    auto missing = config;
    missing.device_name = "__camera_capture_test_missing_device__";
    bool open_failed = false;
    try { capture.open(missing); }
    catch (const std::runtime_error&) { open_failed = true; }
    check(open_failed, "missing camera unexpectedly opened");
    capture.close();
    rtsp::CameraFrame retained;
    std::uint8_t first_byte = 0;
    for (int run = 0; run < 3; ++run) {
        capture.open(config);
        rtsp::CameraFrame first;
        check(capture.read(first, std::chrono::seconds(5)) == rtsp::CaptureResult::FrameReady,
            "could not capture first frame");
        check(first.width == 1280 && first.height == 720 && first.pixel_format == "yuyv422",
            "unexpected frame format");
        check(first.source_pts && first.relative_pts_us == 0 && first.time_base_den > 0,
            "missing source timestamp or nonzero first relative timestamp");
        check(first.linesize[0] >= 1280 * 2, "invalid YUYV stride");
        if (run == 0) {
            retained = first;
            first_byte = retained.buffer->data[0][0];
        }
        rtsp::CameraFrame next;
        check(capture.read(next, std::chrono::seconds(5)) == rtsp::CaptureResult::FrameReady,
            "could not capture next frame");
        check(next.sequence == 1 && next.source_pts > first.source_pts,
            "frame sequence or timestamp did not advance");
        check(retained.buffer->data[0][0] == first_byte, "next read invalidated retained frame");
        bool timeout_rejected = false;
        try { capture.read(next, std::chrono::milliseconds(-1)); }
        catch (const std::invalid_argument&) { timeout_rejected = true; }
        check(timeout_rejected, "negative read timeout was accepted");
        bool timed_out = false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            next.width = 123;
            const auto polled = capture.read(next, std::chrono::milliseconds(0));
            check(polled == rtsp::CaptureResult::FrameReady || polled == rtsp::CaptureResult::Timeout,
                "nonblocking poll unexpectedly failed");
            if (polled == rtsp::CaptureResult::Timeout) {
                check(!next.buffer && next.width == 0 && !next.source_pts,
                    "timeout returned stale frame data");
                timed_out = true;
                break;
            }
        }
        check(timed_out, "nonblocking poll did not expose no-frame state");
        auto stopper = std::async(std::launch::async, [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto stopped_at = std::chrono::steady_clock::now();
            capture.request_stop();
            return stopped_at;
        });
        rtsp::CaptureResult result;
        do { result = capture.read(next, std::chrono::seconds(5)); }
        while (result == rtsp::CaptureResult::FrameReady);
        const auto stopped_at = stopper.get();
        check(result == rtsp::CaptureResult::Stopped, "stop did not wake capture reader");
        check(std::chrono::steady_clock::now() - stopped_at < std::chrono::seconds(1),
            "capture stop exceeded deadline");
        check(capture.read(next, std::chrono::milliseconds(1)) == rtsp::CaptureResult::Stopped,
            "capture did not remain stopped");
        capture.close();
        check(retained.buffer->data[0][0] == first_byte, "close invalidated retained frame");
    }
    retained = {};
}

}

int main(int argc, char* argv[]) {
    try {
        configuration_tests();
        if (argc == 3 && std::string(argv[1]) == "--device") { hardware_tests(argv[2]); }
        else if (argc != 1) { throw std::invalid_argument("usage: camera_capture_tests [--device NAME]"); }
        std::cout << "camera_capture_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "camera_capture_tests failed: " << error.what() << '\n';
        return 1;
    }
}
