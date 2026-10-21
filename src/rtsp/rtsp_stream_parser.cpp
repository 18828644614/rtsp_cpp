#include "rtsp/rtsp_stream_parser.h"

#include <stdexcept>

namespace rtsp {

void RtspStreamParser::append(const char* data, std::size_t size) {
    if (data == nullptr && size != 0) {
        throw std::invalid_argument("append requires valid data");
    }
    // 入口每次读一小块，并及时调用 next 消费，最多保留 2 MiB 未处理数据。
    const std::size_t max_buffer_size = 2 * 1024 * 1024;
    if (size > max_buffer_size - buffer_.size()) {
        throw std::runtime_error("RTSP stream buffer is too large");
    }
    if (size != 0) {
        buffer_.append(data, size);
    }
}

bool RtspStreamParser::next(RtspStreamItem& item) {
    if (buffer_.empty()) {
        return false;
    }

    RtspStreamItem parsed;
    std::size_t consumed = 0;
    if (buffer_[0] == '$') {
        // 只看当前消息起点，不在二进制负载里搜索 '$'。
        if (buffer_.size() < 4) {
            return false;
        }
        const unsigned int high = static_cast<unsigned char>(buffer_[2]);
        const unsigned int low = static_cast<unsigned char>(buffer_[3]);
        const std::size_t payload_size = high * 256 + low;
        if (buffer_.size() - 4 < payload_size) {
            return false;
        }
        parsed.is_interleaved = true;
        parsed.channel = static_cast<unsigned char>(buffer_[1]);
        parsed.payload = buffer_.substr(4, payload_size);
        consumed = 4 + payload_size;
    } else {
        if (!parse_rtsp_message(buffer_, parsed.message, consumed)) {
            return false;
        }
    }

    item = parsed;
    buffer_.erase(0, consumed);  // 剩下的字节保留给下一次 next。
    return true;
}

void RtspStreamParser::finish() const {
    if (!buffer_.empty()) {
        throw std::runtime_error("connection ended with an incomplete RTSP message or frame");
    }
}

std::string serialize_interleaved_frame(unsigned int channel, const std::string& payload) {
    if (channel > 255 || payload.size() > 65535) {
        throw std::invalid_argument("interleaved channel or payload is too large");
    }
    std::string frame;
    frame += '$';
    frame += static_cast<char>(channel);
    frame += static_cast<char>(payload.size() / 256);
    frame += static_cast<char>(payload.size() % 256);
    frame += payload;
    return frame;
}

}  // namespace rtsp
