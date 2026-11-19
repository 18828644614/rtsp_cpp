#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace rtsp::app {

enum class ServerAction { Help, ListCameras, ListMicrophones, Stream };

struct ServerOptions {
    ServerAction action = ServerAction::Stream;
    int camera_index = -1;
    int microphone_index = -1;
    std::uint16_t port = 8554;
    std::string address = "127.0.0.1";
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 2000000;
    int gop = 0;  // 默认一秒一个关键帧，解析完成后取 fps。
    std::string pixel_format = "yuyv422";
    int send_buffer_ms = 150;
};

inline int server_number(const std::string& value, int minimum, int maximum,
                         const std::string& option) {
    const char* end = value.data() + value.size();
    int number = 0;
    const auto parsed = std::from_chars(value.data(), end, number);
    if (value.empty() || value[0] == '-' || parsed.ec != std::errc{} || parsed.ptr != end ||
        number < minimum || number > maximum) {
        throw std::invalid_argument("invalid value for " + option + ": " + value);
    }
    return number;
}

// 不带参数只显示用法，避免意外打开摄像头或麦克风。
inline ServerOptions parse_server_options(const std::vector<std::string>& args) {
    ServerOptions options;
    if (args.empty() || (args.size() == 1 && args[0] == "--help")) {
        options.action = ServerAction::Help;
        return options;
    }
    if (args.size() == 1 && args[0] == "--list-cameras") {
        options.action = ServerAction::ListCameras;
        return options;
    }
    if (args.size() == 1 && args[0] == "--list-microphones") {
        options.action = ServerAction::ListMicrophones;
        return options;
    }

    std::set<std::string> seen;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& option = args[i];
        if (!seen.insert(option).second) {
            throw std::invalid_argument("duplicate option: " + option);
        }
        if (option != "--camera" && option != "--microphone" && option != "--port" &&
            option != "--bind" && option != "--width" && option != "--height" &&
            option != "--fps" && option != "--bitrate" && option != "--gop" &&
            option != "--pixel-format" && option != "--send-buffer-ms") {
            throw std::invalid_argument("unknown option: " + option);
        }
        if (++i == args.size() || args[i].empty() || args[i].compare(0, 2, "--") == 0) {
            throw std::invalid_argument("missing value for " + option);
        }
        const auto& value = args[i];
        if (option == "--camera") {
            options.camera_index = server_number(value, 0, 10000, option);
        } else if (option == "--microphone") {
            options.microphone_index = server_number(value, 0, 10000, option);
        } else if (option == "--port") {
            options.port = static_cast<std::uint16_t>(server_number(value, 1, 65535, option));
        } else if (option == "--bind") {
            options.address = value;
        } else if (option == "--width") {
            options.width = server_number(value, 2, 16384, option);
        } else if (option == "--height") {
            options.height = server_number(value, 2, 16384, option);
        } else if (option == "--fps") {
            options.fps = server_number(value, 1, 240, option);
        } else if (option == "--bitrate") {
            options.bitrate = server_number(value, 1, std::numeric_limits<int>::max(), option);
        } else if (option == "--gop") {
            options.gop = server_number(value, 1, 10000, option);
        } else if (option == "--pixel-format") {
            options.pixel_format = value;
        } else if (option == "--send-buffer-ms") {
            options.send_buffer_ms = server_number(value, 0, 200, option);
        }
    }
    if (options.camera_index < 0 || options.microphone_index < 0) {
        throw std::invalid_argument("streaming requires both --camera INDEX and --microphone INDEX");
    }
    if (options.width % 2 || options.height % 2) {
        throw std::invalid_argument("camera dimensions must be even");
    }
    if (options.pixel_format != "yuyv422" && options.pixel_format != "bgr24") {
        throw std::invalid_argument("pixel format must be yuyv422 or bgr24");
    }
    if (options.gop == 0) {
        options.gop = options.fps;
    }
    return options;
}

}  // namespace rtsp::app
