#include "app/server_options.h"

#include <iostream>

namespace {
void check(bool ok) {
    if (!ok) {
        throw std::runtime_error("server options mismatch");
    }
}

void rejects(const std::vector<std::string>& args) {
    try {
        rtsp::app::parse_server_options(args);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("accepted invalid server options");
}
}

int main() {
    try {
        using rtsp::app::parse_server_options;
        using rtsp::app::ServerAction;
        check(parse_server_options({}).action == ServerAction::Help);
        check(parse_server_options({"--help"}).action == ServerAction::Help);
        check(parse_server_options({"--list-cameras"}).action == ServerAction::ListCameras);
        check(parse_server_options({"--list-microphones"}).action == ServerAction::ListMicrophones);

        const std::vector<std::string> devices{"--camera", "0", "--microphone", "1"};
        const auto defaults = parse_server_options(devices);
        check(defaults.action == ServerAction::Stream && defaults.camera_index == 0 &&
              defaults.microphone_index == 1 && defaults.port == 8554 && defaults.send_buffer_ms == 150);
        const auto custom = parse_server_options({"--microphone", "2", "--camera", "3",
            "--width", "640", "--height", "480", "--fps", "25", "--bitrate", "1000000",
            "--pixel-format", "bgr24", "--bind", "0.0.0.0", "--port", "9000", "--send-buffer-ms", "0"});
        check(custom.camera_index == 3 && custom.microphone_index == 2 && custom.width == 640 &&
              custom.height == 480 && custom.fps == 25 && custom.gop == 25 &&
              custom.bitrate == 1000000 && custom.pixel_format == "bgr24" &&
              custom.address == "0.0.0.0" && custom.port == 9000 && custom.send_buffer_ms == 0);
        for (const auto& invalid : std::vector<std::vector<std::string>>{
                 {"--width", "641"}, {"--height", "0"}, {"--fps", "241"},
                 {"--bitrate", "2147483648"}, {"--gop", "0"}, {"--pixel-format", "mjpeg"},
                 {"--port", "0"}, {"--port", "999999999999999999"}, {"--send-buffer-ms", "201"},
                 {"--camera", "1"}, {"--synthetic-video"}, {"--synthetic-audio"},
                 {"--list-cameras"}, {"--list-microphones"}, {"--help"}, {"--port"}, {"--bind", ""}}) {
            auto args = devices;
            args.insert(args.end(), invalid.begin(), invalid.end());
            rejects(args);
        }
        for (const auto& invalid : std::vector<std::vector<std::string>>{
                 {"--camera", "0"}, {"--microphone", "0"}, {"--microphone", "-1"},
                 {"--camera", "1x"}, {"--camera", "-0"}, {"--camera", "--fps", "30"},
                 {"8554", "127.0.0.1", "input.h264", "input.aac"}}) {
            rejects(invalid);
        }
        std::cout << "PASS: only a camera and microphone pair can start streaming\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
