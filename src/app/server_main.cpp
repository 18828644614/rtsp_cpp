#include "app/server_options.h"
#include "rtsp/rtsp_server.h"
#ifdef RTSP_HAS_CAMERA_CAPTURE
#include "media/camera_capture.h"
#endif
#ifdef RTSP_HAS_CAMERA_SOURCE
#include "media/camera_capture_input.h"
#endif

#include <exception>
#include <iostream>
#include <string>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <windows.h>

namespace {

std::atomic<bool> console_stop{false};

BOOL WINAPI on_console_control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        console_stop.store(true);  // 控制处理线程只置标记，不接触 Socket 和对象生命周期。
        return TRUE;
    }
    return FALSE;
}

void print_usage() {
    std::cout << "Usage: rtsp_server [port] [bind_address] [input.h264|-] [input.aac]\n"
              << "Defaults: 8554 127.0.0.1 samples/sample.h264\n"
              << "Video: Annex-B with AUD, SPS/PPS; fixed 25 fps. Use - for audio only.\n"
              << "Audio: optional ADTS AAC-LC, fixed sample rate/channels, 1024 samples/AU.\n"
              << "Camera: rtsp_server --camera INDEX | --device NAME [options]\n"
              << "  --list-cameras              Enumerate devices without opening them\n"
              << "  --port N --bind IPv4         Defaults 8554 127.0.0.1\n"
              << "  --width N --height N         Even dimensions; defaults 1280 720\n"
              << "  --fps N --bitrate N --gop N  Defaults 30, 2000000 bits/s, one-second GOP\n"
              << "  --pixel-format NAME         yuyv422 (default) or bgr24\n"
              << "Camera streaming requires ENABLE_CAMERA_CAPTURE=ON and ENABLE_H264_ENCODING=ON.\n"
              << "Playback: ffplay -rtsp_transport tcp rtsp://SERVER_IP:8554/live\n";
}

class ConsoleScope {
public:
    ConsoleScope() {
        if (!::SetConsoleCtrlHandler(on_console_control, TRUE)) {
            throw std::runtime_error("could not register console stop handler");
        }
    }
    ~ConsoleScope() { ::SetConsoleCtrlHandler(on_console_control, FALSE); }
};

std::string utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                       nullptr, 0, nullptr, nullptr);
    if (size <= 0) { throw std::invalid_argument("invalid Unicode argument"); }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                        result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index) { args.push_back(utf8(argv[index])); }
        auto options = rtsp::app::parse_server_options(args);
        SetConsoleOutputCP(CP_UTF8);
        if (options.help) {
            print_usage();
            return 0;
        }
        ConsoleScope console;
#ifndef RTSP_HAS_CAMERA_SOURCE
        if (options.camera()) {
            throw std::invalid_argument(
                "camera streaming requires ENABLE_CAMERA_CAPTURE=ON and ENABLE_H264_ENCODING=ON");
        }
#endif
#ifdef RTSP_HAS_CAMERA_CAPTURE
        if (options.list_cameras || options.camera_index >= 0) {
            const auto devices = rtsp::CameraCapture::enumerate_devices();
            if (options.list_cameras) {
                for (std::size_t index = 0; index < devices.size(); ++index) {
                    std::cout << '[' << index << "] " << devices[index].description << '\n';
                }
                std::cout << "video_devices=" << devices.size() << '\n';
                return 0;
            }
            if (static_cast<std::size_t>(options.camera_index) >= devices.size()) {
                throw std::invalid_argument("camera index out of range; use --list-cameras");
            }
            options.device_name = devices[static_cast<std::size_t>(options.camera_index)].name;
        }
#else
        if (options.list_cameras) {
            throw std::invalid_argument("--list-cameras requires ENABLE_CAMERA_CAPTURE=ON");
        }
#endif
        std::unique_ptr<rtsp::RtspServer> server;
        if (options.camera()) {
#ifdef RTSP_HAS_CAMERA_SOURCE
            rtsp::CameraConfig capture;
            capture.device_name = options.device_name;
            capture.width = options.width;
            capture.height = options.height;
            capture.fps_num = options.fps;
            capture.pixel_format = options.pixel_format;
            rtsp::CameraVideoSourceConfig source_config;
            source_config.encoder.width = capture.width;
            source_config.encoder.height = capture.height;
            source_config.encoder.fps_num = capture.fps_num;
            source_config.encoder.pixel_format = capture.pixel_format;
            source_config.encoder.bitrate = options.bitrate;
            source_config.encoder.gop = options.gop;
            std::cout << "Opening camera: " << capture.device_name << ", " << capture.width << 'x'
                      << capture.height << " @ " << capture.fps_num << " fps, "
                      << capture.pixel_format << ", bitrate=" << options.bitrate
                      << ", GOP=" << options.gop << std::endl;
            auto source = std::make_unique<rtsp::CameraVideoSource>(
                rtsp::make_camera_capture_input(capture), source_config, &console_stop);
            server = std::make_unique<rtsp::RtspServer>(options.address, options.port, std::move(source));
#else
            throw std::invalid_argument(
                "camera streaming requires ENABLE_CAMERA_CAPTURE=ON and ENABLE_H264_ENCODING=ON");
#endif
        } else {
            server = std::make_unique<rtsp::RtspServer>(options.address, options.port,
                                                       options.video_path, options.audio_path);
        }
        if (!console_stop.load()) { server->run(&console_stop); }
        return 0;
    } catch (const std::exception& error) {
        if (console_stop.load()) { return 0; }
        std::cerr << "Server error: " << error.what() << '\n';
        return 1;
    }
}
