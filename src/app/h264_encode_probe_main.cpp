#include "app/synthetic_camera_frame.h"
#include "media/h264_encoder.h"
#include "media/h264_file_writer.h"
#include "sdp/session_description.h"
#ifdef RTSP_HAS_CAMERA_CAPTURE
#include "media/camera_capture.h"
#endif

#include <windows.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

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
            throw std::runtime_error("register console control handler failed");
        }
    }
    ~ConsoleScope() { SetConsoleCtrlHandler(on_console_control, FALSE); }
};

std::string utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) { throw std::invalid_argument("invalid Unicode argument"); }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

int number(const std::string& text, int minimum, int maximum) {
    if (text.empty()) { throw std::invalid_argument("empty number"); }
    int value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9' || value > (maximum - (ch - '0')) / 10) {
            throw std::invalid_argument("invalid number: " + text);
        }
        value = value * 10 + ch - '0';
    }
    if (value < minimum || value > maximum) { throw std::invalid_argument("number out of range: " + text); }
    return value;
}

void usage() {
    std::cout << "Usage: h264_encode_probe --output FILE [options]\n"
              << "  --synthetic                 Generated input (default), no camera required\n"
              << "  --device NAME / --device-index N  Camera input; requires ENABLE_CAMERA_CAPTURE\n"
              << "  --list                      List cameras without opening them\n"
              << "  --width N --height N        Even dimensions; defaults 1280 720\n"
              << "  --fps N --fps-den N         Frame rate N/den; defaults 30/1\n"
              << "  --pixel-format NAME         yuyv422 (default) or bgr24\n"
              << "  --bitrate N --gop N         Defaults 2000000 bits/s and one-second GOP\n"
              << "  --frames N                  Default 60; 0 captures until Ctrl+C\n"
              << "  --timeout-ms N              Camera no-frame timeout, default 5000\n"
              << "  --output FILE               Annex-B H264; also writes FILE.csv (90 kHz timing)\n"
              << "  --quiet --help\n";
}

}

int wmain(int argc, wchar_t* argv[]) {
    try {
        rtsp::H264EncoderConfig config;
        std::string output_path;
        std::string device;
        int device_index = -1;
        int frames = 60;
        int timeout_ms = 5000;
        bool synthetic_explicit = false;
        bool list = false;
        bool quiet = false;
        bool explicit_gop = false;
        for (int index = 1; index < argc; ++index) {
            const auto option = utf8(argv[index]);
            if (option == "--help") { usage(); return 0; }
            if (option == "--synthetic") { synthetic_explicit = true; continue; }
            if (option == "--list") { list = true; continue; }
            if (option == "--quiet") { quiet = true; continue; }
            if (option != "--width" && option != "--height" && option != "--fps" &&
                option != "--fps-den" && option != "--pixel-format" && option != "--bitrate" &&
                option != "--gop" && option != "--frames" && option != "--timeout-ms" &&
                option != "--output" && option != "--device" && option != "--device-index") {
                throw std::invalid_argument("unknown option: " + option);
            }
            if (++index == argc) { throw std::invalid_argument("missing value for " + option); }
            const auto value = utf8(argv[index]);
            if (option == "--width") { config.width = number(value, 2, 16384); }
            else if (option == "--height") { config.height = number(value, 2, 16384); }
            else if (option == "--fps") { config.fps_num = number(value, 1, 10000000); }
            else if (option == "--fps-den") { config.fps_den = number(value, 1, 10000000); }
            else if (option == "--pixel-format") { config.pixel_format = value; }
            else if (option == "--bitrate") { config.bitrate = number(value, 1, std::numeric_limits<int>::max()); }
            else if (option == "--gop") { config.gop = number(value, 1, 10000); explicit_gop = true; }
            else if (option == "--frames") { frames = number(value, 0, 1000000); }
            else if (option == "--timeout-ms") { timeout_ms = number(value, 1, 60000); }
            else if (option == "--output") { output_path = value; }
            else if (option == "--device") {
                if (value.empty()) { throw std::invalid_argument("empty camera name"); }
                device = value;
            } else if (option == "--device-index") { device_index = number(value, 0, 10000); }
        }
        if (!device.empty() && device_index >= 0) {
            throw std::invalid_argument("--device and --device-index are mutually exclusive");
        }
        const bool camera_input = !device.empty() || device_index >= 0;
        if (synthetic_explicit && (camera_input || list)) {
            throw std::invalid_argument("--synthetic cannot be combined with camera selection");
        }
        SetConsoleOutputCP(CP_UTF8);
#ifdef RTSP_HAS_CAMERA_CAPTURE
        if (list || device_index >= 0) {
            const auto devices = rtsp::CameraCapture::enumerate_devices();
            if (list) {
                for (std::size_t index = 0; index < devices.size(); ++index) {
                    std::cout << '[' << index << "] " << devices[index].description << '\n';
                }
                std::cout << "video_devices=" << devices.size() << '\n';
                return 0;
            }
            if (static_cast<std::size_t>(device_index) >= devices.size()) {
                throw std::invalid_argument("camera index out of range; use --list");
            }
            device = devices[static_cast<std::size_t>(device_index)].name;
        }
#else
        if (list || camera_input) { throw std::invalid_argument("camera input requires ENABLE_CAMERA_CAPTURE=ON"); }
#endif
        if (output_path.empty()) { throw std::invalid_argument("--output FILE is required"); }
        ConsoleScope console;
#ifdef RTSP_HAS_CAMERA_CAPTURE
        rtsp::CameraCapture capture;
        if (camera_input) {
            rtsp::CameraConfig requested;
            requested.device_name = device;
            requested.width = config.width;
            requested.height = config.height;
            requested.fps_num = config.fps_num;
            requested.fps_den = config.fps_den;
            requested.pixel_format = config.pixel_format;
            capture.open(requested);
            const auto actual = capture.format();
            config.fps_num = actual.fps_num;
            config.fps_den = actual.fps_den;
        }
#endif
        if (!explicit_gop) { config.gop = std::max(1, config.fps_num / config.fps_den); }
        rtsp::H264Encoder encoder;
        encoder.open(config);
        rtsp::H264FileWriter writer;
        writer.open(output_path);
        std::ofstream timing(output_path + ".csv", std::ios::trunc);
        if (!timing) { throw std::runtime_error("open timing CSV failed"); }
        timing << "index,pts_90k,duration_90k,idr,nalus,bytes\n";
        std::uint64_t au_count = 0;
        std::uint64_t byte_count = 0;
        const auto save = [&](const std::vector<rtsp::H264AccessUnit>& units) {
            for (const auto& unit : units) {
                std::size_t bytes = 0;
                for (const auto& nalu : unit.nalus) { writer.write(nalu); bytes += nalu.size(); }
                timing << au_count << ',' << unit.pts << ',' << unit.duration << ','
                       << unit.is_keyframe << ',' << unit.nalus.size() << ',' << bytes << '\n';
                if (!timing) { throw std::runtime_error("write timing CSV failed"); }
                if (!quiet || au_count == 0) {
                    std::cout << "[au] n=" << au_count << " pts=" << unit.pts << " duration=" << unit.duration
                              << " idr=" << unit.is_keyframe << " nalus=" << unit.nalus.size() << " bytes=" << bytes << '\n';
                }
                ++au_count;
                byte_count += bytes;
            }
        };
        std::int64_t input_count = 0;
        auto last_frame = std::chrono::steady_clock::now();
        while (!console_stop.load() && (frames == 0 || input_count < frames)) {
            rtsp::CameraFrame frame;
#ifdef RTSP_HAS_CAMERA_CAPTURE
            if (camera_input) {
                const auto result = capture.read(frame, std::chrono::milliseconds(100));
                if (result == rtsp::CaptureResult::Stopped) { break; }
                if (result == rtsp::CaptureResult::Failed) { throw std::runtime_error(capture.last_error()); }
                if (result == rtsp::CaptureResult::Timeout) {
                    if (std::chrono::steady_clock::now() - last_frame >= std::chrono::milliseconds(timeout_ms)) {
                        throw std::runtime_error("camera no-frame timeout");
                    }
                    continue;
                }
            } else
#endif
            {
                frame = rtsp::app::synthetic_camera_frame(config.width, config.height,
                    config.pixel_format, config.fps_num, config.fps_den, input_count);
            }
            last_frame = std::chrono::steady_clock::now();
            save(encoder.encode(frame));
            ++input_count;
        }
#ifdef RTSP_HAS_CAMERA_CAPTURE
        capture.request_stop();
        capture.close();
#else
        (void)timeout_ms;
        (void)last_frame;
#endif
        save(encoder.flush());
        if (au_count != static_cast<std::uint64_t>(input_count)) { throw std::runtime_error("input/output frame count mismatch"); }
        if (au_count) { std::cout << "[encoder] fmtp=" << rtsp::make_h264_fmtp(encoder.sps(), encoder.pps()) << '\n'; }
        timing.close();
        if (!timing) { throw std::runtime_error("close timing CSV failed"); }
        writer.close();
        std::cout << "[encoder] input=" << (camera_input ? "camera" : "synthetic")
                  << " frames=" << input_count << " access_units=" << au_count << " nalu_bytes=" << byte_count
                  << " timestamp_mode=" << (encoder.timestamp_mode() == rtsp::EncoderTimestampMode::Source ? "source" :
                      encoder.timestamp_mode() == rtsp::EncoderTimestampMode::FrameRate ? "frame_rate" : "unset")
                  << " timestamp_fallbacks=" << encoder.timestamp_fallbacks()
                  << " sps_bytes=" << encoder.sps().size() << " pps_bytes=" << encoder.pps().size()
                  << " reason=" << (console_stop.load() ? "console_stop" : "frame_limit") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Encode error: " << error.what() << '\n';
        return 1;
    }
}
