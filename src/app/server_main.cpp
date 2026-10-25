#include "app/command_line.h"
#include "rtsp/rtsp_server.h"

#include <exception>
#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cout << "Usage: rtsp_server [port] [bind_address] [input.h264]\n"
              << "Defaults: 8554 127.0.0.1 samples/sample.h264\n"
              << "Input: Annex-B with AUD, SPS/PPS; fixed 25 access units per second.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            print_usage();
            return 0;
        }
        if (argc > 4) {
            print_usage();
            return 1;
        }
        const std::uint16_t port = argc >= 2 ? rtsp::app::parse_port(argv[1]) : 8554;
        const std::string address = argc >= 3 ? argv[2] : "127.0.0.1";
        const std::string path = argc >= 4 ? argv[3] : "samples/sample.h264";
        rtsp::RtspServer server(address, port, path);
        server.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Server error: " << error.what() << '\n';
        return 1;
    }
}
