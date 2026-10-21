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
        std::cout << "RTSP demo server listening on " << address << ':'
                  << listener.local_port() << std::endl;

        // 2. 新建空的连接对象，accept 把新连接写入这个对象。
        // listener 继续持有监听句柄，connection 持有通信句柄。
        rtsp::net::TcpSocket connection;
        listener.accept_client(connection);
        std::cout << "Client connected." << std::endl;

        // 3. 先收字节，再拆消息。一轮读取可能得到半条、一条或多条消息。
        rtsp::RtspStreamParser parser;
        rtsp::RtspStreamItem item;
        char buffer[4096];
        for (;;) {
            const std::size_t count = connection.receive(buffer, sizeof(buffer));
            if (count == 0) {
                parser.finish();  // 检查是否有被截断的消息残留。
                break;
            }
            parser.append(buffer, count);
            while (parser.next(item)) {
                if (item.is_interleaved) {
                    // 解析器已经可以识别交织帧，但本阶段还没有协商媒体通道。
                    throw std::runtime_error("unexpected media frame before SETUP");
                }
                const rtsp::RtspMessage& request = item.message;
                if (!request.is_request) {
                    throw std::runtime_error("server expected an RTSP request");
                }
                std::cout << "Request: " << request.method << ", CSeq="
                          << request.header("CSeq") << std::endl;

                // 当前只提供最小 OPTIONS 演示，完整会话流程留到下一步。
                rtsp::RtspMessage response;
                response.is_request = false;
                response.set_header("CSeq", request.header("CSeq"));
                if (request.method == "OPTIONS") {
                    response.set_header("Public", "OPTIONS");
                } else {
                    response.status_code = 501;
                    response.reason = "Not Implemented";
                }
                const std::string wire = rtsp::serialize_rtsp_message(response);
                connection.send_all(wire.data(), wire.size());
            }
        }

        connection.shutdown_send();
        std::cout << "Client finished.\n";
        // 4. 本样例只服务一个连接，处理结束后关闭连接和监听端口。
        connection.close();
        listener.close();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Server error: " << error.what() << '\n';
        return 1;
    }
}
