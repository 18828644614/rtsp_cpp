#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rtsp {

// 限制来自网络的数据长度，防止错误的对端让缓冲区无限增长。
constexpr std::size_t kMaxRtspHeaderSize = 16 * 1024;
constexpr std::size_t kMaxRtspBodySize = 1024 * 1024;

struct RtspHeader {
    std::string name;
    std::string value;
};

// 请求和响应共用头字段与消息体，只有起始行不同。
struct RtspMessage {
    bool is_request = true;

    // 请求起始行，例如：OPTIONS rtsp://127.0.0.1:8554/live RTSP/1.0
    std::string method;
    std::string uri;

    // 响应起始行，例如：RTSP/1.0 200 OK
    int status_code = 200;
    std::string reason = "OK";

    std::vector<RtspHeader> headers;
    std::string body;  // 按字节保存，也可以包含零字节和 '$'。

    // 头字段名不区分大小写；未找到时返回空字符串。
    std::string header(const std::string& name) const;
    void set_header(const std::string& name, const std::string& value);
};

// 从 data 开头解析一条文本消息，后面可以有其他消息。
// 完整：返回 true，并通过 consumed 告诉调用方消费了多少字节。
// 不完整：返回 false，consumed 为 0，message 保持不变。
// 非法：抛出异常。当前要求每条请求和响应都携带数字 CSeq。
bool parse_rtsp_message(const std::string& data, RtspMessage& message,
                        std::size_t& consumed);

// 封装一个rtsp消息
// 自动按 body 的实际字节数生成 Content-Length，不相信手工填写的长度。
std::string serialize_rtsp_message(const RtspMessage& message);

}  // namespace rtsp
