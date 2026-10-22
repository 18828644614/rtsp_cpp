#pragma once

#include "net/tcp_socket.h"
#include "rtsp/rtsp_session_utils.h"
#include "rtsp/rtsp_stream_parser.h"
#include "sdp/session_description.h"

namespace rtsp {

class RtspClient {
public:
    explicit RtspClient(net::TcpSocket& connection);

    // 在已经连接的 Socket 上顺序完成五种请求，不在每次请求后关闭连接。
    void run(const std::string& url);

    SessionDescription description() const;
    TcpTransport transport() const;
    std::string session_id() const;

private:
    RtspMessage exchange(RtspMessage request);
    void check_session(const RtspMessage& response) const;

    // 引用只借用入口程序的 Socket，Socket 的生命周期仍由入口程序管理。
    net::TcpSocket& connection_;
    RtspStreamParser parser_;
    int next_cseq_ = 1;
    std::string session_id_;
    SessionDescription description_;
    TcpTransport transport_;
};

}  // namespace rtsp
