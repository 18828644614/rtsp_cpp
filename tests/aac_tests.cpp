#include "media/aac_packetizer.h"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        const rtsp::AacPacketizer packetizer;
        const rtsp::AacAccessUnit frame{std::string("\x11\x22\x33", 3), -1024, 1024};
        const auto unit = packetizer.packetize(frame, 48000);
        if (unit.payloads.size() != 1 || unit.payloads[0] != std::string("\0\x10\0\x18\x11\x22\x33", 7)
            || unit.pts != -1024 || unit.clock_rate != 48000 || unit.is_keyframe) {
            throw std::runtime_error("AAC-hbr AU headers or timestamp mismatch");
        }
        for (const auto& bytes : {std::string{}, std::string(8192, 'x')}) {
            bool rejected = false;
            try {
                packetizer.packetize({bytes, 0, 1024}, 48000);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            if (!rejected) {
                throw std::runtime_error("invalid AAC AU size accepted");
            }
        }
        std::cout << "PASS: AAC-hbr AU header, RTP clock and size limit\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
