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
                handle_interleaved(item);
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

void RtspClient::handle_interleaved(const RtspStreamItem& item) {
    if (session_id_.empty()) {
        throw std::runtime_error("unexpected interleaved frame before SETUP");
    }
    if (item.channel == static_cast<unsigned int>(transport_.rtcp_channel)) {
        return;
    }
    if (item.channel != static_cast<unsigned int>(transport_.rtp_channel)) {
        throw std::runtime_error("frame arrived on an unnegotiated channel");
    }
    const RtpPacket packet = parse_rtp_packet(item.payload);
    if (packet.payload_type != static_cast<unsigned int>(video_.payload_type)) {
        throw std::runtime_error("RTP payload type does not match SDP");
    }
    if (received_packet_ && (packet.sequence_number != next_sequence_ || packet.ssrc != ssrc_)) {
        throw std::runtime_error("RTP sequence gap or changed SSRC");
    }
    received_packet_ = true;
    next_sequence_ = static_cast<std::uint16_t>(packet.sequence_number + 1);
    ssrc_ = packet.ssrc;
    std::vector<std::string> nalus;
    const H264DepacketizeResult result = depacketizer_.push(packet, nalus);
    if (result == H264Dropped) {
        throw std::runtime_error("incomplete H264 NALU was dropped");
    }
    for (std::size_t index = 0; index < nalus.size(); ++index) {
        writer_.write(nalus[index]);
    }
    frame_complete_ = packet.marker;
    if (packet.marker) {
        depacketizer_.finish();
        ++received_frames_;
    }
}

void RtspClient::receive_media(unsigned int frame_limit) {
    char buffer[4096];
    RtspStreamItem item;
    for (;;) {
        while (parser_.next(item)) {
            if (!item.is_interleaved) {
                throw std::runtime_error("unexpected RTSP message during media reception");
            }
            handle_interleaved(item);
            if (frame_limit != 0 && received_frames_ >= frame_limit && frame_complete_) {
                return;
            }
        }
        const std::size_t count = connection_.receive(buffer, sizeof(buffer));
        if (count == 0) {
            parser_.finish();
            depacketizer_.finish();
            if (!received_packet_ || !frame_complete_) {
                throw std::runtime_error("stream ended before a complete H264 access unit");
            }
            return;
        }
        parser_.append(buffer, count);
    }
}

void RtspClient::run(const std::string& url, const std::string& output_path,
                     unsigned int frame_limit) {
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
    bool found_video = false;
    for (const MediaTrack& track : description_.tracks) {
        if (track.media_type == "video" && track.encoding == "H264") {
            video_ = track;
            found_video = true;
            break;
        }
    }
    if (!found_video) {
        throw std::runtime_error("this file-saving client requires an H264 video track");
    }
    std::string base = describe_response.header("Content-Base");
    if (base.empty()) {
        base = describe_response.header("Content-Location");
    }
    if (base.empty()) {
        base = url;
    } else {
        base = resolve_control_url(url, base);
    }
    const std::string track_url = resolve_control_url(base, video_.control);
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
    std::cout << "Video: " << video_.encoding << '/' << video_.clock_rate
              << ", payload=" << video_.payload_type << "\nTrack: " << track_url
              << "\nSession: " << session_id_ << "\nChannels: " << transport_.rtp_channel
              << '/' << transport_.rtcp_channel << '\n';

    writer_.open(output_path);
    RtspMessage play;
    play.method = "PLAY";
    play.uri = aggregate_url;
    play.set_header("Session", session_id_);
    check_session(exchange(play));
    std::cout << "PLAY succeeded; receiving H264.\n";
    receive_media(frame_limit);

    if (frame_limit != 0 && received_frames_ >= frame_limit) {
        RtspMessage teardown;
        teardown.method = "TEARDOWN";
        teardown.uri = aggregate_url;
        teardown.set_header("Session", session_id_);
        check_session(exchange(teardown));
    }
    writer_.close();
    std::cout << "Saved " << output_path << ", access units=" << received_frames_ << '\n';
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
