#include "app/server_options.h"
#include "media/audio_source.h"
#include "media/capture_timeline.h"
#include "media/video_source.h"
#include "rtsp/rtsp_server.h"
#ifdef RTSP_HAS_AUDIO_SOURCE
#include "app/synthetic_audio_input.h"
#include "media/microphone_audio_source.h"
#endif
#ifdef RTSP_HAS_MICROPHONE_CAPTURE
#include "media/microphone_capture_input.h"
#endif
#ifdef RTSP_HAS_CAMERA_CAPTURE
#include "media/camera_capture.h"
#endif
#ifdef RTSP_HAS_VIDEO_SOURCE
#include "app/synthetic_camera_input.h"
#ifdef RTSP_HAS_CAMERA_CAPTURE
#include "media/camera_capture_input.h"
#endif
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
              << "Video: --camera INDEX | --device NAME | --synthetic-video\n"
              << "  --list-cameras              Enumerate devices without opening them\n"
              << "  --port N --bind IPv4         Defaults 8554 127.0.0.1\n"
              << "  --width N --height N         Even dimensions; defaults 1280 720\n"
              << "  --fps N --bitrate N --gop N  Defaults 30, 2000000 bits/s, one-second GOP\n"
              << "  --pixel-format NAME         yuyv422 (default) or bgr24\n"
              << "Camera streaming requires ENABLE_CAMERA_CAPTURE=ON and ENABLE_H264_ENCODING=ON.\n"
              << "Audio: --microphone INDEX | --microphone-device NAME | --synthetic-audio\n"
              << "  --list-microphones          Enumerate microphones (requires capture and AAC options)\n"
              << "  Audio streaming requires ENABLE_AAC_ENCODING=ON; microphone also needs ENABLE_MICROPHONE_CAPTURE=ON.\n"
              << "Dual track: combine a video and an audio input. --send-buffer-ms 0..200 (default 150).\n"
              << "Without devices: rtsp_server --synthetic-video --synthetic-audio\n"
              << "Playback: ffplay -rtsp_transport tcp rtsp://SERVER_IP:8554/live\n";
}

class ConsoleScope {
public:
    ConsoleScope() {
        if (!::SetConsoleCtrlHandler(on_console_control, TRUE)) {
            throw std::runtime_error("could not register console stop handler");
        }
    }
    ~ConsoleScope() {
        ::SetConsoleCtrlHandler(on_console_control, FALSE);
    }
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

std::unique_ptr<rtsp::VideoSource> make_video_source(
    const rtsp::app::ServerOptions& options,
    const std::shared_ptr<const rtsp::CaptureTimeline>& timeline) {
#ifdef RTSP_HAS_VIDEO_SOURCE
    rtsp::CameraVideoSourceConfig config;
    config.timeline = timeline;
    config.encoder.width = options.width;
    config.encoder.height = options.height;
    config.encoder.fps_num = options.fps;
    config.encoder.pixel_format = options.pixel_format;
    config.encoder.bitrate = options.bitrate;
    config.encoder.gop = options.gop;
    std::unique_ptr<rtsp::CameraFrameInput> input;
    if (options.synthetic_video) {
        input = std::make_unique<rtsp::app::SyntheticCameraInput>(config.encoder);
    } else {
#ifdef RTSP_HAS_CAMERA_CAPTURE
        rtsp::CameraConfig capture;
        capture.device_name = options.device_name;
        if (options.camera_index >= 0) {
            const auto devices = rtsp::CameraCapture::enumerate_devices();
            if (static_cast<std::size_t>(options.camera_index) >= devices.size()) {
                throw std::invalid_argument("camera index out of range; use --list-cameras");
            }
            capture.device_name = devices[static_cast<std::size_t>(options.camera_index)].name;
        }
        capture.width = options.width;
        capture.height = options.height;
        capture.fps_num = options.fps;
        capture.pixel_format = options.pixel_format;
        std::cout << "Opening camera: " << capture.device_name << std::endl;
        input = rtsp::make_camera_capture_input(capture);
#else
        throw std::invalid_argument("camera streaming requires ENABLE_CAMERA_CAPTURE=ON");
#endif
    }
    return std::make_unique<rtsp::CameraVideoSource>(std::move(input), config, &console_stop);
#else
    (void)options;
    (void)timeline;
    throw std::invalid_argument("video streaming requires ENABLE_H264_ENCODING=ON (camera also needs ENABLE_CAMERA_CAPTURE=ON)");
#endif
}

std::unique_ptr<rtsp::AudioSource> make_audio_source(
    const rtsp::app::ServerOptions& options,
    const std::shared_ptr<const rtsp::CaptureTimeline>& timeline) {
#ifdef RTSP_HAS_AUDIO_SOURCE
    std::unique_ptr<rtsp::AudioFrameInput> input;
    if (options.synthetic_audio) {
        input = std::make_unique<rtsp::app::SyntheticAudioInput>(0, 44100, 2, true);
    } else {
#ifdef RTSP_HAS_MICROPHONE_CAPTURE
        rtsp::MicrophoneConfig capture;
        capture.device_name = options.microphone_device;
        if (options.microphone_index >= 0) {
            const auto devices = rtsp::MicrophoneCapture::enumerate_devices();
            if (static_cast<std::size_t>(options.microphone_index) >= devices.size()) {
                throw std::invalid_argument("microphone index out of range; use --list-microphones");
            }
            capture.device_name = devices[static_cast<std::size_t>(options.microphone_index)].name;
        }
        input = rtsp::make_microphone_capture_input(capture);
#else
        throw std::invalid_argument("microphone streaming requires ENABLE_MICROPHONE_CAPTURE=ON");
#endif
    }
    rtsp::MicrophoneAudioSourceConfig config;
    config.timeline = timeline;
    if (timeline) {
        // Whole 1024-sample AUs within the 200 ms queue budget at 48 kHz.
        config.queue.max_frames = 9;
    }
    auto source = std::make_unique<rtsp::MicrophoneAudioSource>(std::move(input), config, &console_stop);
    std::cout << "Audio: AAC-LC 48000 Hz mono, " << source->track().fmtp << std::endl;
    return source;
#else
    (void)options;
    (void)timeline;
    throw std::invalid_argument("audio streaming requires ENABLE_AAC_ENCODING=ON");
#endif
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index) {
            args.push_back(utf8(argv[index]));
        }
        const auto options = rtsp::app::parse_server_options(args);
        SetConsoleOutputCP(CP_UTF8);
        if (options.help) {
            print_usage();
            return 0;
        }
        ConsoleScope console;
        if (options.list_microphones) {
#ifdef RTSP_HAS_MICROPHONE_CAPTURE
            const auto devices = rtsp::MicrophoneCapture::enumerate_devices();
            for (std::size_t index = 0; index < devices.size(); ++index) {
                std::cout << index << ": " << devices[index].name << '\n';
            }
            return 0;
#else
            throw std::invalid_argument("microphone enumeration requires ENABLE_MICROPHONE_CAPTURE=ON and ENABLE_AAC_ENCODING=ON");
#endif
        }
        if (options.list_cameras) {
#ifdef RTSP_HAS_CAMERA_CAPTURE
            const auto devices = rtsp::CameraCapture::enumerate_devices();
            for (std::size_t index = 0; index < devices.size(); ++index) {
                std::cout << '[' << index << "] " << devices[index].description << '\n';
            }
            std::cout << "video_devices=" << devices.size() << '\n';
            return 0;
#else
            throw std::invalid_argument("--list-cameras requires ENABLE_CAMERA_CAPTURE=ON");
#endif
        }
        std::unique_ptr<rtsp::RtspServer> server;
        if (options.camera() || options.audio()) {
            const auto timeline = options.camera() && options.audio()
                ? std::make_shared<const rtsp::CaptureTimeline>() : nullptr;
            auto video = options.camera() ? make_video_source(options, timeline) : nullptr;
            auto audio = options.audio() ? make_audio_source(options, timeline) : nullptr;
            if (timeline) {
                video->check_health();
                audio->check_health();
            }
            server = std::make_unique<rtsp::RtspServer>(options.address, options.port,
                std::move(video), std::move(audio), timeline,
                std::chrono::milliseconds{options.send_buffer_ms});
        } else {
            server = std::make_unique<rtsp::RtspServer>(options.address, options.port,
                                                       options.video_path, options.audio_path);
        }
        if (!console_stop.load()) {
            server->run(&console_stop);
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
