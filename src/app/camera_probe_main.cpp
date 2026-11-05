#include "media/camera_capture.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
std::atomic<bool> console_stop{false};

BOOL WINAPI on_console_control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        console_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

class ConsoleScope {
public:
    ConsoleScope() {
        if (!SetConsoleCtrlHandler(on_console_control, TRUE)) {
            throw std::runtime_error("could not register console stop handler");
        }
    }
    ~ConsoleScope() { SetConsoleCtrlHandler(on_console_control, FALSE); }
};

std::string utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) { throw std::invalid_argument("invalid Unicode argument"); }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

int number(const std::string& text, int minimum, int maximum, const std::string& option) {
    if (text.empty()) { throw std::invalid_argument("missing number for " + option); }
    int value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9' || value > (maximum - (digit - '0')) / 10) {
            throw std::invalid_argument("invalid number for " + option);
        }
        value = value * 10 + (digit - '0');
    }
    if (value < minimum || value > maximum) {
        throw std::invalid_argument("number out of range for " + option);
    }
    return value;
}

void usage() {
    std::cout << "Usage: camera_probe [options]\n"
              << "  --list                       Enumerate video devices without opening them\n"
              << "  --device NAME                Friendly name or alternative name (UTF-8)\n"
              << "  --device-index N             Select index from --list (mutually exclusive with --device)\n"
              << "  --width N --height N         Defaults: 1280 720\n"
              << "  --fps N --pixel-format NAME  Defaults: 30 yuyv422 (also supports bgr24)\n"
              << "  --frames N                   Frames per run; default 1, 0 means unlimited\n"
              << "  --seconds N                  Duration per run; implies unlimited frames unless --frames is set\n"
              << "  --timeout-ms N               Fail after this long without a frame; default 5000\n"
              << "  --repeat N                   Close and reopen for each run; default 1\n"
              << "  --quiet                      Print first frame and summary only\n"
              << "  --help                       Show this help\n"
              << "Default device: Integrated Camera. Ctrl+C / Ctrl+Break stops capture.\n";
}

void print_frame(const rtsp::CameraFrame& frame) {
    std::cout << "[frame] n=" << frame.sequence << " size=" << frame.width << 'x' << frame.height
              << " fmt=" << frame.pixel_format << " linesize=";
    for (std::size_t index = 0; index < frame.linesize.size(); ++index) {
        std::cout << (index ? "," : "") << frame.linesize[index];
    }
    std::cout << " source_pts=";
    if (frame.source_pts) { std::cout << *frame.source_pts; } else { std::cout << "missing"; }
    std::cout << " time_base=" << frame.time_base_num << '/' << frame.time_base_den
              << " source_duration=" << frame.source_duration << " relative_pts_us=";
    if (frame.relative_pts_us) { std::cout << *frame.relative_pts_us; } else { std::cout << "missing"; }
    std::cout << " arrival_steady_us=" << frame.arrival_steady_us << '\n';
}

}

int wmain(int argc, wchar_t* argv[]) {
    try {
        rtsp::CameraConfig config;
        bool list = false;
        bool quiet = false;
        bool explicit_device = false;
        bool explicit_frames = false;
        int device_index = -1;
        int frames = 1;
        int seconds = 0;
        int timeout_ms = 5000;
        int repeat = 1;
        for (int index = 1; index < argc; ++index) {
            const std::string option = utf8(argv[index]);
            if (option == "--help") { usage(); return 0; }
            if (option == "--list") { list = true; continue; }
            if (option == "--quiet") { quiet = true; continue; }
            if (option != "--device" && option != "--device-index" && option != "--width" &&
                option != "--height" && option != "--fps" && option != "--pixel-format" &&
                option != "--frames" && option != "--seconds" && option != "--timeout-ms" &&
                option != "--repeat") {
                throw std::invalid_argument("unknown option: " + option);
            }
            if (++index == argc) { throw std::invalid_argument("missing value for " + option); }
            const std::string value = utf8(argv[index]);
            if (option == "--device") { config.device_name = value; explicit_device = true; }
            else if (option == "--device-index") { device_index = number(value, 0, 10000, option); }
            else if (option == "--width") { config.width = number(value, 1, 16384, option); }
            else if (option == "--height") { config.height = number(value, 1, 16384, option); }
            else if (option == "--fps") { config.fps_num = number(value, 1, 240, option); }
            else if (option == "--pixel-format") { config.pixel_format = value; }
            else if (option == "--frames") { frames = number(value, 0, 1000000, option); explicit_frames = true; }
            else if (option == "--seconds") { seconds = number(value, 1, 86400, option); }
            else if (option == "--timeout-ms") { timeout_ms = number(value, 1, 60000, option); }
            else if (option == "--repeat") { repeat = number(value, 1, 1000, option); }
        }
        if (explicit_device && device_index >= 0) {
            throw std::invalid_argument("--device and --device-index are mutually exclusive");
        }
        SetConsoleOutputCP(CP_UTF8);
        std::cout << "[camera] FFmpeg " << rtsp::CameraCapture::ffmpeg_version() << '\n';
        if (list || device_index >= 0) {
            const auto devices = rtsp::CameraCapture::enumerate_devices();
            if (list) {
                for (std::size_t index = 0; index < devices.size(); ++index) {
                    std::cout << '[' << index << "] " << devices[index].description
                              << "\n    id=" << devices[index].name << '\n';
                }
                std::cout << "[camera] video_devices=" << devices.size() << '\n';
                return 0;
            }
            if (static_cast<std::size_t>(device_index) >= devices.size()) {
                throw std::invalid_argument("camera index out of range; use --list");
            }
            config.device_name = devices[static_cast<std::size_t>(device_index)].name;
        }
        if (seconds && !explicit_frames) { frames = 0; }
        ConsoleScope console;
        rtsp::CameraCapture capture;
        for (int run = 1; run <= repeat && !console_stop.load(); ++run) {
            std::cout << "[camera] run=" << run << " requested: device=" << config.device_name
                      << " size=" << config.width << 'x' << config.height
                      << " fps=" << config.fps_num << '/' << config.fps_den
                      << " fmt=" << config.pixel_format << '\n';
            capture.open(config);
            const auto actual = capture.format();
            std::cout << "[camera] negotiated: codec=" << actual.codec
                      << " size=" << actual.width << 'x' << actual.height
                      << " fps=" << actual.fps_num << '/' << actual.fps_den
                      << " fmt=" << actual.pixel_format << " time_base="
                      << actual.time_base_num << '/' << actual.time_base_den << '\n';
            const auto started = Clock::now();
            auto last_frame = started;
            std::uint64_t count = 0;
            std::optional<std::int64_t> first_pts_us;
            std::optional<std::int64_t> last_pts_us;
            bool failed = false;
            std::string reason = "frame_limit";
            while (!frames || count < static_cast<std::uint64_t>(frames)) {
                if (console_stop.load()) { reason = "console_stop"; break; }
                if (seconds && Clock::now() - started >= std::chrono::seconds(seconds)) {
                    reason = "duration_limit";
                    break;
                }
                rtsp::CameraFrame frame;
                const auto result = capture.read(frame, std::chrono::milliseconds(100));
                if (result == rtsp::CaptureResult::FrameReady) {
                    last_frame = Clock::now();
                    if (!quiet || count == 0) { print_frame(frame); }
                    if (frame.relative_pts_us) {
                        if (!first_pts_us) { first_pts_us = frame.relative_pts_us; }
                        last_pts_us = frame.relative_pts_us;
                    }
                    ++count;
                } else if (result == rtsp::CaptureResult::Stopped) {
                    reason = "requested_stop";
                    break;
                } else if (result == rtsp::CaptureResult::Failed) {
                    std::cerr << "[camera] " << capture.last_error() << '\n';
                    failed = true;
                    reason = "input_failed";
                    break;
                } else if (Clock::now() - last_frame >= std::chrono::milliseconds(timeout_ms)) {
                    std::cerr << "[camera] no frame for " << timeout_ms << " ms (capture stalled)\n";
                    failed = true;
                    reason = "no_frame_timeout";
                    break;
                }
            }
            const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
            capture.request_stop();
            capture.close();
            std::cout << std::fixed << std::setprecision(3)
                      << "[camera] released run=" << run << " reason=" << reason
                      << " frames=" << count << " elapsed_s=" << elapsed
                      << " received_fps=" << (elapsed > 0 ? count / elapsed : 0.0);
            if (count > 1 && first_pts_us && last_pts_us && *last_pts_us > *first_pts_us) {
                std::cout << " source_fps=" << (count - 1) * 1000000.0 / (*last_pts_us - *first_pts_us);
            }
            std::cout << '\n';
            if (failed) { return 1; }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Camera error: " << error.what() << '\n';
        return 1;
    }
}
