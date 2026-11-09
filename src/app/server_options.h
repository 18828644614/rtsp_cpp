#pragma once

#include "app/command_line.h"

#include <limits>
#include <set>
#include <vector>

namespace rtsp::app {

// 不依赖 FFmpeg 的启动配置，默认构建也能验证参数并报告功能未启用。
struct ServerOptions {
    std::uint16_t port = 8554;
    std::string address = "127.0.0.1";
    std::string video_path = "samples/sample.h264";
    std::string audio_path;
    int camera_index = -1;
    std::string device_name;
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 2000000;
    int gop = 30;
    std::string pixel_format = "yuyv422";
    bool list_cameras = false;
    bool help = false;
    bool camera() const { return camera_index >= 0 || !device_name.empty(); }
};

inline int server_number(const std::string& value, int minimum, int maximum,
                         const std::string& option) {
    int result = 0;
    if (value.empty()) { throw std::invalid_argument("empty value for " + option); }
    for (const char digit : value) {
        if (digit < '0' || digit > '9' || result > (maximum - (digit - '0')) / 10) {
            throw std::invalid_argument("invalid value for " + option + ": " + value);
        }
        result = result * 10 + digit - '0';
    }
    if (result < minimum || result > maximum) {
        throw std::invalid_argument("value out of range for " + option);
    }
    return result;
}

// args 不含程序名；文件模式保留原有位置参数，摄像头模式使用命名参数。
inline ServerOptions parse_server_options(const std::vector<std::string>& args) {
    ServerOptions options;
    if (args.empty()) { return options; }
    if (args.size() == 1 && args[0] == "--help") { options.help = true; return options; }
    if (args[0].compare(0, 2, "--") != 0) {
        if (args.size() > 4) { throw std::invalid_argument("too many file-mode arguments"); }
        for (const auto& value : args) {
            if (value.compare(0, 2, "--") == 0) {
                throw std::invalid_argument("cannot mix file arguments and camera options");
            }
        }
        options.port = parse_port(args[0]);
        if (args.size() >= 2) { options.address = args[1]; }
        if (args.size() >= 3) { options.video_path = args[2] == "-" ? "" : args[2]; }
        if (args.size() >= 4) { options.audio_path = args[3]; }
        return options;
    }

    std::set<std::string> seen;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& option = args[index];
        if (!seen.insert(option).second) { throw std::invalid_argument("duplicate option: " + option); }
        if (option == "--list-cameras") { options.list_cameras = true; continue; }
        if (option != "--camera" && option != "--device" && option != "--port" &&
            option != "--bind" && option != "--width" && option != "--height" &&
            option != "--fps" && option != "--bitrate" && option != "--gop" &&
            option != "--pixel-format") {
            throw std::invalid_argument("unknown option: " + option);
        }
        if (++index == args.size()) { throw std::invalid_argument("missing value for " + option); }
        const auto& value = args[index];
        if (value.empty() || value.compare(0, 2, "--") == 0) {
            throw std::invalid_argument("missing value for " + option);
        }
        if (option == "--camera") { options.camera_index = server_number(value, 0, 10000, option); }
        else if (option == "--device") { options.device_name = value; }
        else if (option == "--port") { options.port = parse_port(value); }
        else if (option == "--bind") { options.address = value; }
        else if (option == "--width") { options.width = server_number(value, 2, 16384, option); }
        else if (option == "--height") { options.height = server_number(value, 2, 16384, option); }
        else if (option == "--fps") { options.fps = server_number(value, 1, 240, option); }
        else if (option == "--bitrate") {
            options.bitrate = server_number(value, 1, std::numeric_limits<int>::max(), option);
        } else if (option == "--gop") { options.gop = server_number(value, 1, 10000, option); }
        else if (option == "--pixel-format") { options.pixel_format = value; }
    }
    if (options.list_cameras) {
        if (seen.size() != 1) { throw std::invalid_argument("--list-cameras must be used alone"); }
        return options;
    }
    if (!options.camera() || (options.camera_index >= 0 && !options.device_name.empty())) {
        throw std::invalid_argument("select exactly one of --camera INDEX or --device NAME");
    }
    if (options.width % 2 || options.height % 2) {
        throw std::invalid_argument("camera dimensions must be even");
    }
    if (options.pixel_format != "yuyv422" && options.pixel_format != "bgr24") {
        throw std::invalid_argument("pixel format must be yuyv422 or bgr24");
    }
    if (!seen.count("--gop")) { options.gop = options.fps; }
    options.video_path.clear();
    return options;
}

}  // namespace rtsp::app
