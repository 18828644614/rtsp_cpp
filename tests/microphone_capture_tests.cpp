#include "media/microphone_capture.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using Result = rtsp::MicrophoneReadResult;

void check(bool valid, const char* message) {
    if (!valid) {
        throw std::runtime_error(message);
    }
}

template <typename Action>
void rejects(Action action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "invalid microphone configuration was accepted");
}

void configuration_tests() {
    rtsp::MicrophoneCapture capture;
    rtsp::MicrophoneConfig config;
    rejects([&] {
        capture.open(config);
    });
    config.device_name = "__microphone_test_missing_device__";
    config.sample_rate = -1;
    rejects([&] {
        capture.open(config);
    });
    config.sample_rate = 384001;
    rejects([&] {
        capture.open(config);
    });
    config.sample_rate = 0;
    config.channels = -1;
    rejects([&] {
        capture.open(config);
    });
    config.channels = 65;
    rejects([&] {
        capture.open(config);
    });
    config.channels = 0;
    config.audio_buffer_ms = -1;
    rejects([&] {
        capture.open(config);
    });
    config.audio_buffer_ms = 10001;
    rejects([&] {
        capture.open(config);
    });
    config.audio_buffer_ms = 0;
    rtsp::AudioFrame frame;
    frame.sample_rate = 123;
    check(capture.read(frame, std::chrono::milliseconds(0)) == Result::Failed
              && frame.sample_rate == 0 && !frame.buffer && !capture.last_error().empty(),
          "unopened read did not clear output and report an error");
    rejects([&] {
        capture.read(frame, std::chrono::milliseconds(-1));
    });
    capture.request_stop();
    check(capture.read(frame, std::chrono::milliseconds(1)) == Result::Stopped,
          "stop request was not observable");
    capture.close();
    capture.close();
    check(capture.format().sample_rate == 0, "close retained negotiated format");
    std::atomic<bool> cancelled{true};
    bool cancelled_open = false;
    try {
        capture.open(config, &cancelled);
    } catch (const std::runtime_error& error) {
        cancelled_open = std::string(error.what()) == "microphone open cancelled";
    }
    check(cancelled_open, "open overwrote an external stop request");
    bool missing = false;
    try {
        capture.open(config);
    } catch (const std::runtime_error&) {
        missing = true;
    }
    check(missing && !capture.last_error().empty() && capture.format().sample_rate == 0,
          "missing microphone did not report a clean failure");
    capture.close();
}

// 只在显式指定设备时执行；默认 CTest 不打开麦克风。
void hardware_tests(const std::string& device) {
    rtsp::MicrophoneConfig config;
    config.device_name = device;
    rtsp::MicrophoneCapture capture;
    rtsp::AudioFrame retained;
    std::uint8_t first_byte = 0;
    for (int run = 0; run < 3; ++run) {
        capture.open(config);
        const auto actual = capture.format();
        rtsp::AudioFrame first;
        check(capture.read(first, std::chrono::seconds(5)) == Result::FrameReady,
              "could not capture first PCM frame");
        check(first.buffer && first.buffer->extended_data[0] && first.nb_samples > 0
                  && first.sample_rate == actual.sample_rate && first.channels == actual.channels
                  && first.sample_format == actual.sample_format && first.sequence == 0
                  && first.time_base_num > 0 && first.time_base_den > 0,
              "invalid PCM frame or negotiated format");
        if (run == 0) {
            retained = first;
            first_byte = first.buffer->extended_data[0][0];
        }
        rtsp::AudioFrame next;
        check(capture.read(next, std::chrono::seconds(5)) == Result::FrameReady
                  && next.sequence == 1 && next.arrival_steady_us >= first.arrival_steady_us,
              "PCM capture did not advance");
        if (first.source_pts && next.source_pts) {
            check(*next.source_pts > *first.source_pts, "PCM timestamp did not advance");
        }
        check(retained.buffer->extended_data[0][0] == first_byte, "read invalidated retained PCM");
        bool timed_out = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            next.nb_samples = 123;
            const auto result = capture.read(next, std::chrono::milliseconds(0));
            check(result == Result::FrameReady || result == Result::Timeout,
                  "nonblocking PCM poll failed");
            if (result == Result::Timeout) {
                check(!next.buffer && next.nb_samples == 0 && !next.source_pts,
                      "timeout returned stale PCM");
                timed_out = true;
                break;
            }
        }
        check(timed_out, "zero-timeout PCM polling did not expose no-data state");
        auto stopper = std::async(std::launch::async, [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto stopped_at = Clock::now();
            capture.request_stop();
            return stopped_at;
        });
        Result result;
        do {
            result = capture.read(next, std::chrono::seconds(5));
        } while (result == Result::FrameReady);
        const auto stopped_at = stopper.get();
        check(result == Result::Stopped && Clock::now() - stopped_at < std::chrono::seconds(1),
              "stop did not wake PCM reader within one second");
        const auto close_start = Clock::now();
        capture.close();
        check(retained.buffer->extended_data[0][0] == first_byte, "close invalidated retained PCM");
        std::cout << "hardware run=" << run + 1
                  << " stop_ms=" << std::chrono::duration<double, std::milli>(close_start - stopped_at).count()
                  << " close_ms=" << std::chrono::duration<double, std::milli>(Clock::now() - close_start).count()
                  << '\n';
    }
    // 显式验证外部停止标记；取消之后仍由本线程关闭设备。
    std::atomic<bool> cancelled{false};
    capture.open(config, &cancelled);
    auto stopper = std::async(std::launch::async, [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        cancelled.store(true);
    });
    rtsp::AudioFrame frame;
    Result result;
    do {
        result = capture.read(frame, std::chrono::seconds(5));
    } while (result == Result::FrameReady);
    stopper.get();
    check(result == Result::Stopped, "external stop request did not end read");
    capture.close();
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        configuration_tests();
        if (argc == 3 && std::string(argv[1]) == "--device-index") {
            const std::string value = argv[2];
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
                throw std::invalid_argument("invalid microphone index");
            }
            const auto devices = rtsp::MicrophoneCapture::enumerate_devices();
            const auto index = static_cast<std::size_t>(std::stoul(value));
            check(index < devices.size(), "microphone index out of range");
            hardware_tests(devices[index].name);
        } else if (argc != 1) {
            throw std::invalid_argument("usage: microphone_capture_tests [--device-index N]");
        }
        std::cout << "microphone_capture_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "microphone_capture_tests failed: " << error.what() << '\n';
        return 1;
    }
}
