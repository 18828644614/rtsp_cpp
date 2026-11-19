#include "app/server_options.h"
#include "media/camera_capture_input.h"
#include "media/microphone_capture_input.h"
#include "rtsp/rtsp_server.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

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
            throw std::runtime_error("could not register console stop handler");
        }
    }
    ~ConsoleScope() { SetConsoleCtrlHandler(on_console_control, FALSE); }
};

std::string utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                       nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::invalid_argument("invalid Unicode argument");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                        result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

void print_usage() {
    std::cout << "Usage: rtsp_server --camera INDEX --microphone INDEX [options]\n"
              << "  --list-cameras | --list-microphones   List device indexes\n"
              << "  --port N --bind IPv4                 Default: 127.0.0.1:8554\n"
              << "  --width N --height N --fps N          Default: 1280x720 at 30 fps\n"
              << "  --pixel-format yuyv422|bgr24         Default: yuyv422\n"
              << "  --bitrate N --gop N                   Default: 2000000 bps, 1-second GOP\n"
              << "  --send-buffer-ms 0..200              Default: 150\n"
              << "Playback: ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live\n";
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            args.push_back(utf8(argv[i]));
        }
        const auto options = rtsp::app::parse_server_options(args);
        using rtsp::app::ServerAction;
        SetConsoleOutputCP(CP_UTF8);
        if (options.action == ServerAction::Help) {
            print_usage();
            return 0;
        }
        if (options.action == ServerAction::ListCameras) {
            const auto devices = rtsp::CameraCapture::enumerate_devices();
            for (std::size_t i = 0; i < devices.size(); ++i) {
                std::cout << i << ": " << devices[i].description << '\n';
            }
            std::cout << "video_devices=" << devices.size() << '\n';
            return 0;
        }
        if (options.action == ServerAction::ListMicrophones) {
            const auto devices = rtsp::MicrophoneCapture::enumerate_devices();
            for (std::size_t i = 0; i < devices.size(); ++i) {
                std::cout << i << ": " << devices[i].description << '\n';
            }
            std::cout << "audio_devices=" << devices.size() << '\n';
            return 0;
        }

        ConsoleScope console;
        const auto cameras = rtsp::CameraCapture::enumerate_devices();
        const auto camera_index = static_cast<std::size_t>(options.camera_index);
        if (camera_index >= cameras.size()) {
            throw std::invalid_argument("camera index out of range; use --list-cameras");
        }
        const auto microphones = rtsp::MicrophoneCapture::enumerate_devices();
        const auto microphone_index = static_cast<std::size_t>(options.microphone_index);
        if (microphone_index >= microphones.size()) {
            throw std::invalid_argument("microphone index out of range; use --list-microphones");
        }

        rtsp::CameraConfig camera;
        camera.device_name = cameras[camera_index].name;
        camera.width = options.width;
        camera.height = options.height;
        camera.fps_num = options.fps;
        camera.pixel_format = options.pixel_format;
        rtsp::MicrophoneConfig microphone;
        microphone.device_name = microphones[microphone_index].name;

        // 同一个 T0 同时交给采集端和调度器，不能为音频和视频分别归零。
        const rtsp::CaptureTimeline timeline;
        rtsp::CameraVideoSourceConfig video_config;
        video_config.timeline = timeline;
        video_config.encoder.width = camera.width;
        video_config.encoder.height = camera.height;
        video_config.encoder.fps_num = camera.fps_num;
        video_config.encoder.pixel_format = camera.pixel_format;
        video_config.encoder.bitrate = options.bitrate;
        video_config.encoder.gop = options.gop;
        rtsp::MicrophoneAudioSourceConfig audio_config;
        audio_config.timeline = timeline;

        auto video = std::make_unique<rtsp::CameraVideoSource>(
            rtsp::make_camera_capture_input(camera), video_config, &console_stop);
        auto audio = std::make_unique<rtsp::MicrophoneAudioSource>(
            rtsp::make_microphone_capture_input(microphone), audio_config, &console_stop);
        rtsp::RtspServer server(options.address, options.port, std::move(video), std::move(audio),
                                timeline, std::chrono::milliseconds{options.send_buffer_ms});
        if (!console_stop.load()) {
            server.run(&console_stop);
        }
        return 0;
    } catch (const std::exception& error) {
        if (console_stop.load()) {
            return 0;
        }
        std::cerr << "Server error: " << error.what() << '\n';
        return 1;
    }
}
