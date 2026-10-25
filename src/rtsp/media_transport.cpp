#include "rtsp/media_transport.h"

#include "rtsp/rtsp_stream_parser.h"

namespace rtsp {

MediaTransport::MediaTransport(TcpTransport tcp_transport)
    : tcp_(tcp_transport) {}

TransportMode MediaTransport::mode() const {
    return mode_;
}

const TcpTransport& MediaTransport::tcp() const {
    return tcp_;
}

std::string MediaTransport::frame_rtp(const std::string& payload) const {
    return serialize_interleaved_frame(static_cast<unsigned int>(tcp_.rtp_channel), payload);
}

std::string MediaTransport::frame_rtcp(const std::string& payload) const {
    return serialize_interleaved_frame(static_cast<unsigned int>(tcp_.rtcp_channel), payload);
}

}  // namespace rtsp
