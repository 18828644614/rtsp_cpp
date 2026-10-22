#include "app/command_line.h"
#include "media/h264_file_reader.h"
#include "media/h264_packetizer.h"
#include "net/tcp_socket.h"
#include "rtp/rtp_packet.h"
#include "rtsp/rtsp_message.h"
#include "rtsp/rtsp_server_session.h"
#include "rtsp/rtsp_stream_parser.h"

#include <chrono>
#include <ctime>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_usage() {
    std::cout << "Usage: rtsp_server [port] [bind_address] [input.h264]\n"
              << "Defaults: 8554 127.0.0.1 samples/sample.h264\n"
              << "Input: Annex-B with AUD, SPS/PPS; fixed 25 access units per second.\n";
}

void send_frame(rtsp::net::TcpSocket& connection, const rtsp::H264AccessUnit& frame,
                const rtsp::TcpTransport& transport, int payload_type,
                std::uint16_t& sequence, std::uint32_t timestamp) {
    rtsp::H264Packetizer packetizer;
    for (std::size_t nalu_index = 0; nalu_index < frame.nalus.size(); ++nalu_index) {
        const std::vector<std::string> payloads = packetizer.packetize(frame.nalus[nalu_index]);
        for (std::size_t fragment = 0; fragment < payloads.size(); ++fragment) {
            rtsp::RtpPacket packet;
            packet.payload_type = static_cast<unsigned int>(payload_type);
            packet.sequence_number = sequence;
            sequence = static_cast<std::uint16_t>(sequence + 1);
            packet.timestamp = timestamp;
            packet.ssrc = 12345;
            packet.marker = nalu_index + 1 == frame.nalus.size()
                            && fragment + 1 == payloads.size();
            packet.payload = payloads[fragment];
            const std::string wire = rtsp::serialize_interleaved_frame(
                transport.rtp_channel, rtsp::serialize_rtp_packet(packet));
            connection.send_all(wire.data(), wire.size());
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            print_usage();
            return 0;
        }
        if (argc > 4) {
            print_usage();
            return 1;
        }
        const std::uint16_t port = argc >= 2 ? rtsp::app::parse_port(argv[1]) : 8554;
        const std::string address = argc >= 3 ? argv[2] : "127.0.0.1";
        const std::string path = argc >= 4 ? argv[3] : "samples/sample.h264";
        rtsp::H264FileReader reader;
        reader.open(path);
        rtsp::SessionDescription description;
        description.video.fmtp = rtsp::make_h264_fmtp(reader.sps(), reader.pps());
        std::cout << "Input: " << path << ", access units=" << reader.frame_count() << '\n';

        // 1. 创建监听对象，内部依次执行 socket、bind、listen。
        rtsp::net::TcpSocket listener;
        listener.listen(address, port);
        std::cout << "RTSP server listening on " << address << ':'
                  << listener.local_port() << std::endl;

        // 2. 新建空的连接对象，accept 把新连接写入这个对象。
        // listener 继续持有监听句柄，connection 持有通信句柄。
        rtsp::net::TcpSocket connection;
        listener.accept_client(connection);
        std::cout << "Client connected." << std::endl;

        // 本步是单连接演示，以启动时刻构造 Session 标识，不用于认证。
        // 后续多客户端版本再加入独立且不可预测的会话标识生成器。
        rtsp::RtspServerSession session(std::to_string(std::time(nullptr)), description);

        // 3. 先收字节，再拆消息，再交给状态机生成响应。
        rtsp::RtspStreamParser parser;
        rtsp::RtspStreamItem item;
        char buffer[4096];
        std::uint16_t sequence = 100;
        std::uint32_t timestamp = 0;
        std::size_t sent_frames = 0;
        std::chrono::steady_clock::time_point next_send;
        while (session.state() != rtsp::SessionClosed) {
            if (session.state() == rtsp::SessionPlaying) {
                const auto now = std::chrono::steady_clock::now();
                int wait_ms = 0;
                if (now < next_send) {
                    wait_ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(next_send - now).count()) + 1;
                }
                if (!connection.wait_readable(wait_ms)) {
                    rtsp::H264AccessUnit frame;
                    if (!reader.read(frame)) {
                        std::cout << "File completed, sent access units=" << sent_frames << '\n';
                        connection.shutdown_send();
                        break;
                    }
                    send_frame(connection, frame, session.transport(),
                               description.video.payload_type, sequence, timestamp);
                    timestamp += 3600;
                    ++sent_frames;
                    next_send += std::chrono::milliseconds(40);
                    continue;
                }
            }
            const std::size_t count = connection.receive(buffer, sizeof(buffer));
            if (count == 0) {
                parser.finish();
                break;
            }
            parser.append(buffer, count);
            while (parser.next(item)) {
                if (item.is_interleaved) {
                    if (session.state() == rtsp::SessionInit) {
                        throw std::runtime_error("unexpected media frame before SETUP");
                    }
                    const rtsp::TcpTransport transport = session.transport();
                    if (item.channel != static_cast<unsigned int>(transport.rtp_channel)
                        && item.channel != static_cast<unsigned int>(transport.rtcp_channel)) {
                        throw std::runtime_error("frame arrived on an unnegotiated channel");
                    }
                    continue;
                }
                const rtsp::RtspMessage& request = item.message;
                if (!request.is_request) {
                    throw std::runtime_error("server expected an RTSP request");
                }
                std::cout << "Request: " << request.method << ", CSeq="
                          << request.header("CSeq") << std::endl;

                const rtsp::RtspMessage response = session.handle(request);
                const std::string wire = rtsp::serialize_rtsp_message(response);
                connection.send_all(wire.data(), wire.size());
                std::cout << "Response: " << response.status_code << ' ' << response.reason
                          << std::endl;
                if (request.method == "PLAY" && response.status_code == 200) {
                    next_send = std::chrono::steady_clock::now();
                }
                if (session.state() == rtsp::SessionClosed) {
                    break;  // TEARDOWN 响应已发出，不再等待客户端关闭发送方向。
                }
            }
        }

        std::cout << "Session finished.\n";
        // 4. 本样例只服务一个连接，处理结束后关闭连接和监听端口。
        connection.close();
        listener.close();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Server error: " << error.what() << '\n';
        return 1;
    }
}
