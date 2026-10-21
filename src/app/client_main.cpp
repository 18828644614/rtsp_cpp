#include "app/command_line.h"
#include "net/tcp_socket.h"
#include "rtsp/rtsp_message.h"
#include "rtsp/rtsp_stream_parser.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_usage() {
    std::cout << "Usage: rtsp_client [ipv4_address] [port]\n"
              << "Defaults: ipv4_address=127.0.0.1, port=8554\n"
              << "Send one RTSP OPTIONS request and print the response.\n";
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
        const std::string host = argc >= 2 ? argv[1] : "127.0.0.1";
        const std::uint16_t port = argc >= 3 ? rtsp::app::parse_port(argv[2]) : 8554;

        // 1. 先创建空对象，再调用普通成员函数建立连接。
        rtsp::net::TcpSocket connection;
        connection.connect(host, port);
        std::cout << "Connected to " << host << ':' << port << '\n';
        // 2. 创建请求，由序列化函数补齐起始行、换行和 Content-Length。
        rtsp::RtspMessage request;
        request.method = "OPTIONS";
        request.uri = "rtsp://" + host + ":" + std::to_string(port) + "/live";
        request.set_header("CSeq", "1");
        const std::string wire = rtsp::serialize_rtsp_message(request);
        connection.send_all(wire.data(), wire.size());

        // 3. TCP 只给字节，解析器负责判断响应什么时候完整。
        rtsp::RtspStreamParser parser;
        rtsp::RtspStreamItem item;
        bool got_response = false;
        char buffer[4096];
        for (;;) {
            const std::size_t count = connection.receive(buffer, sizeof(buffer));
            if (count == 0) {
                parser.finish();
                break;
            }
            parser.append(buffer, count);
            while (parser.next(item)) {
                if (item.is_interleaved) {
                    throw std::runtime_error("unexpected media frame before SETUP");
                }
                const rtsp::RtspMessage& response = item.message;
                if (response.is_request || response.header("CSeq") != "1" || got_response) {
                    throw std::runtime_error("unexpected RTSP response or CSeq");
                }
                if (response.status_code != 200) {
                    throw std::runtime_error("OPTIONS failed: " + response.reason);
                }
                got_response = true;
                std::cout << "RTSP/1.0 " << response.status_code << ' ' << response.reason
                          << "\nCSeq: " << response.header("CSeq")
                          << "\nPublic: " << response.header("Public") << '\n';

                // 本演示只做一次交互，收到完整回复后才通知服务器退出。
                // 消息边界由解析器判断，半关闭只用来结束演示连接。
                connection.shutdown_send();
            }
        }
        if (!got_response) {
            throw std::runtime_error("server closed before sending an RTSP response");
        }

        // 4. 显式关闭，方便看到完整流程；忘记调用时析构函数也会关闭。
        connection.close();
        std::cout << "OPTIONS exchange completed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Client error: " << error.what() << '\n';
        return 1;
    }
}
