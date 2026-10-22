#include "app/command_line.h"
#include "net/tcp_socket.h"
#include "rtsp/rtsp_message.h"
#include "rtsp/rtsp_server_session.h"
#include "rtsp/rtsp_stream_parser.h"

#include <ctime>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_usage() {
    std::cout << "Usage: rtsp_server [port] [bind_address]\n"
              << "Defaults: port=8554, bind_address=127.0.0.1\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            print_usage();
            return 0;
        }
        if (argc > 3) {
            print_usage();
            return 1;
        }
        const std::uint16_t port = argc >= 2 ? rtsp::app::parse_port(argv[1]) : 8554;
        const std::string address = argc >= 3 ? argv[2] : "127.0.0.1";

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
        rtsp::RtspServerSession session(std::to_string(std::time(nullptr)));

        // 3. 先收字节，再拆消息，再交给状态机生成响应。
        rtsp::RtspStreamParser parser;
        rtsp::RtspStreamItem item;
        char buffer[4096];
        while (session.state() != rtsp::SessionClosed) {
            const std::size_t count = connection.receive(buffer, sizeof(buffer));
            if (count == 0) {
                parser.finish();  // 检查是否有被截断的消息残留。
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
                    // 这里先跳过已知通道；第六步加入媒体收发，RTCP 单独处理。
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
