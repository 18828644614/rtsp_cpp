#include "app/command_line.h"
#include "net/tcp_socket.h"
#include "rtsp/rtsp_client.h"

#include <exception>
#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cout << "Usage: rtsp_client [ipv4_address] [port]\n"
              << "Defaults: ipv4_address=127.0.0.1, port=8554\n"
              << "Run OPTIONS, DESCRIBE, SETUP, PLAY and TEARDOWN for /live.\n";
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
        // 2. 客户端类组织顺序请求；每次回复仍通过第三步的流解析器读取。
        const std::string url = "rtsp://" + host + ":" + std::to_string(port) + "/live";
        rtsp::RtspClient client(connection);
        client.run(url);

        // 3. TEARDOWN 已完成，直接关闭连接，不再用半关闭代替协议结束。
        connection.close();
        std::cout << "RTSP session completed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Client error: " << error.what() << '\n';
        return 1;
    }
}
