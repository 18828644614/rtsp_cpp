#include "rtsp/rtsp_client.h"

#include <iostream>
#include <stdexcept>

namespace rtsp {

RtspClient::RtspClient(net::TcpSocket& connection) : connection_(connection) {}

RtspMessage RtspClient::exchange(RtspMessage request) {
    const std::string cseq = std::to_string(next_cseq_);
    ++next_cseq_;
    request.set_header("CSeq", cseq);
    const std::string wire = serialize_rtsp_message(request);
    connection_.send_all(wire.data(), wire.size());
    std::cout << "> " << request.method << " " << request.uri << ", CSeq=" << cseq << '\n';

    char buffer[4096];
    RtspStreamItem item;
    for (;;) {
        // 先消费缓冲里的项，不能假定一次 receive 就是一条完整响应。
        while (parser_.next(item)) {
            if (item.is_interleaved) {
                if (session_id_.empty()) {
                    throw std::runtime_error("unexpected interleaved frame before SETUP");
                }
                if (item.channel != static_cast<unsigned int>(transport_.rtp_channel)
                    && item.channel != static_cast<unsigned int>(transport_.rtcp_channel)) {
                    throw std::runtime_error("frame arrived on an unnegotiated channel");
                }
                // 第六步在这里接入 RTP / H.264 收包；RTCP 仍单独分流。
                continue;
            }
            const RtspMessage& response = item.message;
            if (response.is_request || response.header("CSeq") != cseq) {
                throw std::runtime_error("unexpected RTSP response or CSeq");
            }
            if (response.status_code < 200) {
                continue;  // 1xx 为临时响应，继续等待同一请求的最终响应。
            }
            std::cout << "< " << response.status_code << ' ' << response.reason << '\n';
            if (response.status_code != 200) {
                throw std::runtime_error(request.method + " failed: "
                                         + std::to_string(response.status_code) + " " + response.reason);
            }
            return response;
        }

        const std::size_t count = connection_.receive(buffer, sizeof(buffer));
        if (count == 0) {
            parser_.finish();
            throw std::runtime_error("server closed before the expected RTSP response");
        }
        parser_.append(buffer, count);
    }
}

void RtspClient::check_session(const RtspMessage& response) const {
    if (session_id_from_header(response.header("Session")) != session_id_) {
        throw std::runtime_error("server changed or omitted the Session id");
    }
}

void RtspClient::run(const std::string& url) {
    if (next_cseq_ != 1) {
        throw std::runtime_error("create a new client object for another session");
    }
    rtsp_url_path(url);  // 提前检查请求 URL。

    // 1. 查询服务器支持的方法。
    RtspMessage options;
    options.method = "OPTIONS";
    options.uri = url;
    const RtspMessage options_response = exchange(options);
    std::cout << "Public: " << options_response.header("Public") << '\n';

    // 2. 读取 SDP；基地址优先采用 Content-Base，其次 Content-Location，最后请求 URL。
    RtspMessage describe;
    describe.method = "DESCRIBE";
    describe.uri = url;
    describe.set_header("Accept", "application/sdp");
    const RtspMessage describe_response = exchange(describe);
    if (describe_response.header("Content-Type") != "application/sdp") {
        throw std::runtime_error("DESCRIBE did not return application/sdp");
    }
    description_ = parse_sdp(describe_response.body);
    std::string base = describe_response.header("Content-Base");
    if (base.empty()) {
        base = describe_response.header("Content-Location");
    }
    if (base.empty()) {
        base = url;
    } else {
        base = resolve_control_url(url, base);
    }
    const std::string track_url = resolve_control_url(base, description_.video.control);
    // 没有聚合 control 属性时，第一版退回 DESCRIBE 请求 URL。
    const std::string aggregate_url = description_.control.empty()
                                          ? url : resolve_control_url(base, description_.control);

    // 3. 为具体轨道申请通道。必须保存服务器响应中的值，不能写死为 0/1。
    RtspMessage setup;
    setup.method = "SETUP";
    setup.uri = track_url;
    setup.set_header("Transport", make_tcp_transport(transport_));
    const RtspMessage setup_response = exchange(setup);
    TcpTransport negotiated;
    if (!parse_tcp_transport(setup_response.header("Transport"), negotiated)) {
        throw std::runtime_error("SETUP returned an unsupported Transport");
    }
    const std::string session_id = session_id_from_header(setup_response.header("Session"));
    if (session_id.empty() || session_id.find_first_of(" \t\r\n") != std::string::npos) {
        throw std::runtime_error("SETUP did not return a valid Session id");
    }
    session_id_ = session_id;
    transport_ = negotiated;
    std::cout << "Video: " << description_.video.encoding << '/' << description_.video.clock_rate
              << ", payload=" << description_.video.payload_type << "\nTrack: " << track_url
              << "\nSession: " << session_id_ << "\nChannels: " << transport_.rtp_channel
              << '/' << transport_.rtcp_channel << '\n';

    // 4. 对聚合 URL 发 PLAY，后续请求必须携带 SETUP 返回的 Session。
    RtspMessage play;
    play.method = "PLAY";
    play.uri = aggregate_url;
    play.set_header("Session", session_id_);
    check_session(exchange(play));
    std::cout << "PLAY succeeded (control flow only; no media in this step).\n";

    // 5. 本步没有媒体接收循环，直接请求结束会话。
    RtspMessage teardown;
    teardown.method = "TEARDOWN";
    teardown.uri = aggregate_url;
    teardown.set_header("Session", session_id_);
    check_session(exchange(teardown));
}

SessionDescription RtspClient::description() const {
    return description_;
}

TcpTransport RtspClient::transport() const {
    return transport_;
}

std::string RtspClient::session_id() const {
    return session_id_;
}

}  // namespace rtsp
