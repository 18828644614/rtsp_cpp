#pragma once

#include "rtsp/rtsp_message.h"

namespace rtsp {

// 可以表示 RTSP文本消息 或者 TCP交织帧
// RtspStreamItem 只负责保存数据, 只知道：通道号是多少，负载字节是什么
struct RtspStreamItem {
    // false：使用 message；true：使用 channel 和 payload。
    // is_interleaved：true 是tcp交织帧，读取数据：item.channel 和 item.payload，false: rtsp请求或响应 读取成员：item.message
    bool is_interleaved = false;
    RtspMessage message;
    unsigned int channel = 0;
    std::string payload;
};

// 只负责缓冲与拆分，不直接调用 Socket，也不理解 RTP 包里的内容。
// RtspStreamParser 负责把收到的数据拼起来，等完整消息到达后，再输出一个 RtspStreamItem
class RtspStreamParser {
public:
    //把新收到的字节放入内部缓冲区
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
// 创建 tcp 交织帧 '$' + channel + length(2 bytes) + RTP packet
std::string serialize_interleaved_frame(unsigned int channel, const std::string& payload);

}  // namespace rtsp
