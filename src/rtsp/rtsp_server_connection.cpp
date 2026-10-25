#include "rtsp/rtsp_server_connection.h"

#include "media/h264_file_reader.h"
#include "media/h264_packetizer.h"
#include "rtp/rtp_packet.h"
#include "rtsp/rtsp_message.h"
#include "rtsp/rtsp_server_session.h"
#include "rtsp/rtsp_stream_parser.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace rtsp {
namespace {

std::atomic<std::uint64_t> next_session_id{1};
std::atomic<std::uint32_t> next_ssrc{1};

std::string make_session_id() {
    return std::to_string(next_session_id.fetch_add(1, std::memory_order_relaxed));
}

std::uint32_t make_ssrc() {
    std::uint32_t value = next_ssrc.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) {
        value = next_ssrc.fetch_add(1, std::memory_order_relaxed);
    }
    return value;
}

void send_frame(net::TcpSocket& connection, const H264AccessUnit& frame,
                const TcpTransport& transport, int payload_type,
                std::uint16_t& sequence, std::uint32_t timestamp,
                std::uint32_t ssrc) {
    H264Packetizer packetizer;
    for (std::size_t nalu_index = 0; nalu_index < frame.nalus.size(); ++nalu_index) {
        const std::vector<std::string> payloads = packetizer.packetize(frame.nalus[nalu_index]);
        for (std::size_t fragment = 0; fragment < payloads.size(); ++fragment) {
            RtpPacket packet;
            packet.payload_type = static_cast<unsigned int>(payload_type);
            packet.sequence_number = sequence;
            sequence = static_cast<std::uint16_t>(sequence + 1);
            packet.timestamp = timestamp;
            packet.ssrc = ssrc;
            packet.marker = nalu_index + 1 == frame.nalus.size()
                            && fragment + 1 == payloads.size();
            packet.payload = payloads[fragment];
            const std::string wire = serialize_interleaved_frame(
                transport.rtp_channel, serialize_rtp_packet(packet));
            connection.send_all(wire.data(), wire.size());
        }
    }
}

}  // namespace

RtspServerConnection::RtspServerConnection(std::string input_path)
    : input_path_(std::move(input_path)), session_id_(make_session_id()) {}

RtspServerConnection::~RtspServerConnection() {
    stop();
    join();
}

void RtspServerConnection::accept_from(net::TcpSocket& listener) {
    if (worker_.joinable()) {
        throw std::logic_error("connection has already started");
    }
    listener.accept_client(socket_);
}

void RtspServerConnection::start() {
    if (!socket_.is_open()) {
        throw std::logic_error("connection socket is not open");
    }
    if (worker_.joinable()) {
        throw std::logic_error("connection has already started");
    }
    worker_ = std::thread(&RtspServerConnection::run, this);
}

void RtspServerConnection::stop() {
    stop_requested_.store(true, std::memory_order_release);
    socket_.close();
}

void RtspServerConnection::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool RtspServerConnection::finished() const {
    return finished_.load(std::memory_order_acquire);
}

const std::string& RtspServerConnection::session_id() const {
    return session_id_;
}

void RtspServerConnection::run() {
    try {
        H264FileReader reader;
        reader.open(input_path_);
        SessionDescription description;
        description.video.fmtp = make_h264_fmtp(reader.sps(), reader.pps());
        RtspServerSession session(session_id_, description);
        RtspStreamParser parser;
        RtspStreamItem item;
        char buffer[4096];
        std::uint16_t sequence = 100;
        std::uint32_t timestamp = 0;
        const std::uint32_t ssrc = make_ssrc();
        std::size_t sent_frames = 0;
        std::chrono::steady_clock::time_point next_send;

        while (!stop_requested_.load(std::memory_order_acquire)
               && session.state() != SessionClosed) {
            if (session.state() == SessionPlaying) {
                const auto now = std::chrono::steady_clock::now();
                int wait_ms = 0;
                if (now < next_send) {
                    wait_ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(next_send - now).count()) + 1;
                }
                if (!socket_.wait_readable(wait_ms)) {
                    H264AccessUnit frame;
                    if (!reader.read(frame)) {
                        std::cout << "Session " << session_id_ << " completed, access units="
                                  << sent_frames << std::endl;
                        socket_.shutdown_send();
                        break;
                    }
                    send_frame(socket_, frame, session.transport(),
                               description.video.payload_type, sequence, timestamp, ssrc);
                    timestamp += 3600;
                    ++sent_frames;
                    next_send += std::chrono::milliseconds(40);
                    continue;
                }
            }

            const std::size_t count = socket_.receive(buffer, sizeof(buffer));
            if (count == 0) {
                parser.finish();
                break;
            }
            parser.append(buffer, count);
            while (parser.next(item)) {
                if (item.is_interleaved) {
                    if (session.state() == SessionInit) {
                        throw std::runtime_error("unexpected media frame before SETUP");
                    }
                    const TcpTransport transport = session.transport();
                    if (item.channel != static_cast<unsigned int>(transport.rtp_channel)
                        && item.channel != static_cast<unsigned int>(transport.rtcp_channel)) {
                        throw std::runtime_error("frame arrived on an unnegotiated channel");
                    }
                    continue;
                }
                const RtspMessage& request = item.message;
                if (!request.is_request) {
                    throw std::runtime_error("server expected an RTSP request");
                }
                std::cout << "Session " << session_id_ << " request: " << request.method
                          << ", CSeq=" << request.header("CSeq") << std::endl;
                const RtspMessage response = session.handle(request);
                const std::string wire = serialize_rtsp_message(response);
                socket_.send_all(wire.data(), wire.size());
                std::cout << "Session " << session_id_ << " response: " << response.status_code
                          << ' ' << response.reason << std::endl;
                if (request.method == "PLAY" && response.status_code == 200) {
                    next_send = std::chrono::steady_clock::now();
                }
                if (session.state() == SessionClosed) {
                    break;
                }
            }
        }
    } catch (const std::exception& error) {
        if (!stop_requested_.load(std::memory_order_acquire)) {
            std::cerr << "Session " << session_id_ << " error: " << error.what() << '\n';
        }
    }

    socket_.close();
    finished_.store(true, std::memory_order_release);
}

}  // namespace rtsp
