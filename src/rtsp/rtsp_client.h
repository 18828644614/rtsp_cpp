#pragma once

#include "net/tcp_socket.h"
#include "media/h264_depacketizer.h"
#include "media/h264_file_writer.h"
#include "rtsp/rtsp_session_utils.h"
#include "rtsp/rtsp_stream_parser.h"
#include "sdp/session_description.h"
#include <chrono>

namespace rtsp {

class RtspClient {
public:
    explicit RtspClient(net::TcpSocket& connection);

    void run(const std::string& url, const std::string& output_path = "received.h264",
             unsigned int frame_limit = 0);

    SessionDescription description() const;
    TcpTransport transport() const;
    std::string session_id() const;

private:
    RtspMessage exchange(RtspMessage request);
    void check_session(const RtspMessage& response);
    void handle_interleaved(const RtspStreamItem& item);
    void receive_media(unsigned int frame_limit);

    // 引用只借用入口程序的 Socket，Socket 的生命周期仍由入口程序管理。
    net::TcpSocket& connection_;
    RtspStreamParser parser_;
    int next_cseq_ = 1;
    std::string session_id_;
    SessionDescription description_;
    MediaTrack video_;
    TcpTransport transport_;
    H264FileWriter writer_;
    H264Depacketizer depacketizer_;
    bool received_packet_ = false;
    bool frame_complete_ = false;
    std::uint16_t next_sequence_ = 0;
    std::uint32_t ssrc_ = 0;
    unsigned int received_frames_ = 0;
    unsigned int session_timeout_seconds_ = 60;
    std::string aggregate_url_;
    std::string keepalive_method_ = "OPTIONS";
};

}  // namespace rtsp
