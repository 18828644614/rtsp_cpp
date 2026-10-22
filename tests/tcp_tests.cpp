#include "app/command_line.h"
#include "net/tcp_socket.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

using rtsp::net::TcpSocket;

// 不使用 assert，避免 Release 构建关闭检查。
void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_arguments() {
    check(rtsp::app::parse_port("8554") == 8554, "valid port failed");
    check(rtsp::app::parse_port("65535") == 65535, "maximum port failed");
    for (const char* text : {"", "0", "-1", "65536", "8554abc", "999999999999"}) {
        bool rejected = false;
        try {
            rtsp::app::parse_port(text);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "invalid port was accepted");
    }

    TcpSocket client;
    bool rejected = false;
    try {
        client.connect("not-an-ip", 8554);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && !client.is_open(), "invalid IP changed socket state");

    char buffer[8];
    rejected = false;
    try {
        client.receive(buffer, sizeof(buffer));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    check(rejected, "receive on a closed socket succeeded");
}

void test_transfer() {
    TcpSocket listener;
    listener.listen("127.0.0.1", 0);
    const std::uint16_t port = listener.local_port();
    check(port != 0, "system did not assign a port");

    // 测试把两端放在同一线程里，逐块发送、接收和回传，无需学习线程。
    // connect 可先进入监听队列，再由 accept_client 取出连接。
    TcpSocket client;
    client.connect("127.0.0.1", port);
    TcpSocket peer;
    listener.accept_client(peer);
    check(!peer.wait_readable(0), "empty socket reported readable");
    check(!peer.wait_readable(5), "read wait did not time out");
    client.send_all("!", 1);
    check(peer.wait_readable(500), "read wait missed data");
    char ready_byte = 0;
    check(peer.receive(&ready_byte, 1) == 1 && ready_byte == '!', "read wait consumed data");

    std::string payload(2 * 1024 * 1024 + 37, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(i % 251);
    }

    std::string reply;
    char server_buffer[1024];
    char client_buffer[113];
    std::size_t offset = 0;
    while (offset < payload.size()) {
        // 每次只发一小块，防止单线程测试因为一端发满缓冲区而等待。
        std::size_t block_size = payload.size() - offset;
        if (block_size > 4096) {
            block_size = 4096;
        }
        client.send_all(payload.data() + offset, block_size);

        std::string block;
        while (block.size() < block_size) {
            const std::size_t count = peer.receive(server_buffer, sizeof(server_buffer));
            check(count != 0, "server received an early EOF");
            block.append(server_buffer, count);
        }
        peer.send_all(block.data(), block.size());

        std::size_t client_received = 0;
        while (client_received < block_size) {
            const std::size_t count = client.receive(client_buffer, sizeof(client_buffer));
            check(count != 0, "client received an early EOF");
            reply.append(client_buffer, count);
            client_received += count;
        }
        offset += block_size;
    }
    check(reply == payload, "binary echo does not match original data");

    client.send_all(nullptr, 0);  // 空发送是合法的空操作。
    client.shutdown_send();
    check(peer.wait_readable(500), "read wait missed EOF");
    check(peer.receive(server_buffer, sizeof(server_buffer)) == 0, "server missed EOF");

    // 客户端结束发送后仍能收到回复，证明关闭的只是一个方向。
    peer.send_all("!", 1);
    check(client.receive(client_buffer, sizeof(client_buffer)) == 1, "half-close lost reply");
    check(client_buffer[0] == '!', "half-close reply changed");
    peer.shutdown_send();
    check(client.receive(client_buffer, sizeof(client_buffer)) == 0, "client missed EOF");

    client.close();
    client.close();  // 重复关闭安全；同一个对象关闭后可以重新使用。
    check(!client.is_open(), "close failed");
    {
        TcpSocket temporary;
        client.connect("127.0.0.1", port);
        listener.accept_client(temporary);
    }  // 析构函数自动关闭 temporary。
    check(client.receive(client_buffer, sizeof(client_buffer)) == 0, "destructor leaked socket");
}

int main() {
    try {
        test_arguments();
        std::cout << "PASS: ports, invalid address and closed socket\n";
        test_transfer();
        std::cout << "PASS: binary echo, half-close, EOF and automatic cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
