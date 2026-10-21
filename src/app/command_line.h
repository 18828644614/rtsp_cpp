#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace rtsp::app {

// 按位读取十进制端口，例如 "8554"：8 -> 85 -> 855 -> 8554。
inline std::uint16_t parse_port(const std::string& text) {
    if (text.empty()) {
        throw std::invalid_argument("port must be between 1 and 65535");
    }

    unsigned int port = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9') {
            throw std::invalid_argument("port must contain decimal digits only");
        }
        port = port * 10 + (digit - '0');
        // 每读一位就检查，避免很长的数字使 port 溢出。
        if (port > 65535) {
            throw std::invalid_argument("port must be between 1 and 65535");
        }
    }
    if (port == 0) {
        throw std::invalid_argument("port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(port);
}

}  // namespace rtsp::app
