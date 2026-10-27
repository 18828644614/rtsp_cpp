#pragma once

#include <string>
#include <optional>

namespace rtsp {

// 两个协商的交织通道分别承载 RTP 和 RTCP。
struct TcpTransport {
    int rtp_channel = 0;
    int rtcp_channel = 1;
};

// 教学版只接受一个 RTP/AVP/TCP、unicast、interleaved=x-y 方案。
// 非法或不支持时返回 false，不修改 transport。
bool parse_tcp_transport(const std::string& text, TcpTransport& transport);
std::string make_tcp_transport(const TcpTransport& transport);

// 去掉 Session 后的可选参数，例如 "12345;timeout=60" -> "12345"。
std::string session_id_from_header(const std::string& text);

struct SessionHeader {
    std::string id;
    std::optional<unsigned int> timeout_seconds;
};

// timeout 是服务端响应声明的秒数；未知扩展参数跳过，非法/重复 timeout 拒绝。
SessionHeader parse_session_header(const std::string& text);

// 从绝对 RTSP URL 取出资源路径；URL 不合法时抛出异常。
std::string rtsp_url_path(const std::string& url);

// control 支持绝对 URL、根路径、相对路径和聚合控制标记 '*'。
// 相对路径遵循 URL 规则：base 末尾的 '/' 决定追加还是替换最后一段。
std::string resolve_control_url(const std::string& base, const std::string& control);

}  // namespace rtsp
