#pragma once

#include "rtsp/rtsp_message.h"

namespace rtsp {

struct RtspStreamItem {
    // false：使用 message；true：使用 channel 和 payload。
    bool is_interleaved = false;
    RtspMessage message;
    unsigned int channel = 0;
    std::string payload;
};

// 只负责缓冲与拆分，不直接调用 Socket，也不理解 RTP 包里的内容。
class RtspStreamParser {
public:
    void append(const char* data, std::size_t size);

    // 每次取一条，成功返回 true；数据不足返回 false，item 保持不变。
    // 一次 append 后要循环 next，处理可能同时到达的多条消息。
    bool next(RtspStreamItem& item);

    // 对端 EOF 后调用；还有半条消息时抛出异常，不能静默忽略截断。
    void finish() const;

private:
    std::string buffer_;
};

// '$' + 通道号 + 两字节长度（高位在前）+ 负载。
std::string serialize_interleaved_frame(unsigned int channel, const std::string& payload);

}  // namespace rtsp
