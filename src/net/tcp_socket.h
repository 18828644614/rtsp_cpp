#pragma once

#include "net/socket_utils.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace rtsp::net {

// 一个对象管理一个 Socket 句柄。
// 服务器需要两个对象：一个负责监听，一个负责与客户端通信。
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();

    // 不复制 Socket，避免两个对象在析构时重复关闭同一个句柄。
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // 客户端：创建 Socket 并连接服务器。address 只支持数字 IPv4 地址。
    void connect(const std::string& address, std::uint16_t port);

    // 服务器：创建 Socket、绑定地址、开始监听。port=0 由系统分配端口。
    void listen(const std::string& address, std::uint16_t port);

    // 在监听对象上调用，把新连接保存到 client 中。client 必须是空对象。
    void accept_client(TcpSocket& client);

    // 完整发送；一次接收最多返回 capacity 字节，0 表示对端发送结束。
    void send_all(const char* data, std::size_t size);
    std::size_t receive(char* buffer, std::size_t capacity);

    bool wait_readable(int timeout_ms);

    // 结束本端发送，但仍可接收对端数据。
    void shutdown_send();

    // 中断收发但保留句柄；多线程连接先中断，join 后再 close，避免句柄读写竞争。
    void shutdown();

    // 给 socket_ 设置 2000 毫秒（2 秒）的发送超时，防止发送操作一直阻塞
    void set_send_timeout(int timeout_ms);

    // 查询本地端口，主要用于取得系统自动分配的端口。
    std::uint16_t local_port() const;
    bool is_open() const;
    void close();

private:
    SOCKET handle_ = INVALID_SOCKET;
};

}  // namespace rtsp::net
