#include "app/command_line.h"
#include "rtsp/rtsp_server.h"

#include <exception>
#include <iostream>
#include <string>
#include <atomic>
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
              << "Audio: optional ADTS AAC-LC, fixed sample rate/channels, 1024 samples/AU.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            print_usage();
            return 0;
        }
        if (argc > 5) {
            print_usage();
            return 1;
        }
        const std::uint16_t port = argc >= 2 ? rtsp::app::parse_port(argv[1]) : 8554;
        const std::string address = argc >= 3 ? argv[2] : "127.0.0.1";
        std::string video_path = argc >= 4 ? argv[3] : "samples/sample.h264";
        if (video_path == "-") {
            video_path.clear();
        }
        const std::string audio_path = argc >= 5 ? argv[4] : "";
        rtsp::RtspServer server(address, port, video_path, audio_path);
        if (!::SetConsoleCtrlHandler(on_console_control, TRUE)) {
            throw std::runtime_error("could not register console stop handler");
        }
        try {
            server.run(&console_stop);
        } catch (...) {
            ::SetConsoleCtrlHandler(on_console_control, FALSE);
            throw;
        }
        ::SetConsoleCtrlHandler(on_console_control, FALSE);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Server error: " << error.what() << '\n';
        return 1;
    }
}
