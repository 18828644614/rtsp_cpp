#include "net/tcp_socket.h"
#include "net/event_loop.h"

#include <stdexcept>

namespace rtsp::net {

TcpSocket::~TcpSocket() {
    close();
}

void TcpSocket::connect(const std::string& address, std::uint16_t port) {
    if (is_open()) {
        throw std::logic_error("close the existing socket before connecting");
    }
    if (port == 0) {
        throw std::invalid_argument("connection port must not be zero");
    }
    detail::initialize_sockets();

    // 将 IP 字符串和端口转换成 Windows Socket 接口要求的地址结构。
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    if (InetPtonA(AF_INET, address.c_str(), &server.sin_addr) != 1) {
        throw std::invalid_argument("address must be a numeric IPv4 address");
    }

    // 1. 创建 Socket。返回的是 SOCKET 句柄，直接保存到成员变量。
    handle_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!is_open()) {
        detail::throw_socket_error("socket", WSAGetLastError());
    }

    // 2. 连接服务器。失败时先保存错误码，再关闭本次创建的句柄。
    if (::connect(handle_, reinterpret_cast<const sockaddr*>(&server), sizeof(server))
        == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        close();
        detail::throw_socket_error("connect", error);
    }
}

void TcpSocket::listen(const std::string& address, std::uint16_t port) {
    if (is_open()) {
        throw std::logic_error("close the existing socket before listening");
    }
    detail::initialize_sockets();

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (InetPtonA(AF_INET, address.c_str(), &local.sin_addr) != 1) {
        throw std::invalid_argument("address must be a numeric IPv4 address");
    }

    // 1. 创建监听 Socket。
    handle_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!is_open()) {
        detail::throw_socket_error("socket", WSAGetLastError());
    }

    // 2. 绑定本地 IP 和端口。
    if (::bind(handle_, reinterpret_cast<const sockaddr*>(&local), sizeof(local))
        == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        close();
        detail::throw_socket_error("bind", error);
    }

    // 3. 开始监听，随后可以调用 accept_client 等待连接。
    if (::listen(handle_, SOMAXCONN) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        close();
        detail::throw_socket_error("listen", error);
    }
}

void TcpSocket::accept_client(TcpSocket& client) {
    if (!is_open()) {
        throw std::logic_error("listening socket is closed");
    }
    if (client.is_open()) {
        throw std::logic_error("client must be an empty socket");
    }

    // accept 返回新的连接句柄，原来的监听句柄保持不变。
    // 同一个类的成员函数可以访问另一个同类对象的私有成员。
    for (;;) {
        client.handle_ = ::accept(handle_, nullptr, nullptr);
        if (client.is_open()) {
            return;
        }
        const int error = WSAGetLastError();
        if (error != WSAEINTR) {
            detail::throw_socket_error("accept", error);
        }
    }
}

void TcpSocket::send_all(const char* data, std::size_t size) {
    if (!is_open()) {
        throw std::logic_error("sending socket is closed");
    }
    if (data == nullptr && size != 0) {
        throw std::invalid_argument("send buffer must not be null");
    }

    std::size_t sent = 0;
    while (sent < size) {
        // 每次最多提交 64 KiB，保证长度可安全转换为 Winsock 要求的 int。
        const std::size_t remaining = size - sent;
        const int length = remaining > 65536 ? 65536 : static_cast<int>(remaining);
        const int count = ::send(handle_, data + sent, length, 0);
        if (count == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEINTR) {
                continue;
            }
            detail::throw_socket_error("send", error);
        }
        if (count == 0) {
            throw std::runtime_error("send made no progress");
        }
        sent += static_cast<std::size_t>(count);
    }
}

void TcpSocket::set_nonblocking(bool enabled) {
    if (!is_open()) {
        throw std::logic_error("nonblocking requires an open socket");
    }
    u_long mode = enabled ? 1 : 0;
    if (::ioctlsocket(handle_, FIONBIO, &mode) == SOCKET_ERROR) {
        detail::throw_socket_error("ioctlsocket(FIONBIO)", WSAGetLastError());
    }
}

bool TcpSocket::try_accept(TcpSocket& client) {
    if (!is_open() || client.is_open()) {
        throw std::logic_error("accept requires listener and empty client");
    }
    for (;;) {
        client.handle_ = ::accept(handle_, nullptr, nullptr);
        if (client.is_open()) {
            try {
                client.set_nonblocking();
            } catch (...) {
                client.close();
                throw;
            }
            return true;
        }
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAECONNRESET || error == WSAECONNABORTED) {
            // 待接受的连接可能已被对端复位，留到下一轮接受其他连接。
            return false;
        }
        if (error != WSAEINTR) {
            detail::throw_socket_error("accept", error);
        }
    }
}

IoResult TcpSocket::receive_some(char* buffer, std::size_t capacity) {
    if (!is_open() || buffer == nullptr || capacity == 0) {
        throw std::invalid_argument("receive_some requires open socket and buffer");
    }
    const int length = capacity > 65536 ? 65536 : static_cast<int>(capacity);
    for (;;) {
        const int count = ::recv(handle_, buffer, length, 0);
        if (count > 0) {
            return {IoStatus::Done, static_cast<std::size_t>(count)};
        }
        if (count == 0) {
            return {IoStatus::Eof, 0};
        }
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            return {IoStatus::WouldBlock, 0};
        }
        if (error != WSAEINTR) {
            detail::throw_socket_error("recv", error);
        }
    }
}

IoResult TcpSocket::send_some(const char* data, std::size_t size) {
    if (!is_open() || (data == nullptr && size != 0)) {
        throw std::invalid_argument("send_some requires open socket and valid data");
    }
    if (size == 0) {
        return {IoStatus::Done, 0};
    }
    const int length = size > 65536 ? 65536 : static_cast<int>(size);
    for (;;) {
        const int count = ::send(handle_, data, length, 0);
        if (count > 0) {
            return {IoStatus::Done, static_cast<std::size_t>(count)};
        }
        if (count == 0) {
            throw std::runtime_error("send made no progress");
        }
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            return {IoStatus::WouldBlock, 0};
        }
        if (error != WSAEINTR) {
            detail::throw_socket_error("send", error);
        }
    }
}

SOCKET TcpSocket::native_handle() const {
    return handle_;
}

std::size_t TcpSocket::receive(char* buffer, std::size_t capacity) {
    if (!is_open()) {
        throw std::logic_error("receiving socket is closed");
    }
    if (buffer == nullptr || capacity == 0) {
        throw std::invalid_argument("receive requires a nonempty buffer");
    }

    const int length = capacity > 65536 ? 65536 : static_cast<int>(capacity);
    for (;;) {
        const int count = ::recv(handle_, buffer, length, 0);
        if (count != SOCKET_ERROR) {
            // 大于 0：收到的数据长度。等于 0：对端发送结束。
            return static_cast<std::size_t>(count);
        }
        const int error = WSAGetLastError();
        if (error != WSAEINTR) {
            detail::throw_socket_error("recv", error);
        }
    }
}

bool TcpSocket::wait_readable(int timeout_ms) {
    if (!is_open() || timeout_ms < 0) {
        throw std::invalid_argument("wait_readable requires an open socket and nonnegative timeout");
    }
    EventLoop loop;
    loop.watch(handle_, true, false);
    return !loop.wait(timeout_ms).empty();
}

void TcpSocket::shutdown_send() {
    if (!is_open()) {
        throw std::logic_error("socket is closed");
    }
    if (::shutdown(handle_, SD_SEND) == SOCKET_ERROR) {
        detail::throw_socket_error("shutdown", WSAGetLastError());
    }
}

void TcpSocket::shutdown() {
    if (is_open()) {
        ::shutdown(handle_, SD_BOTH);
    }
}

void TcpSocket::set_send_timeout(int timeout_ms) {
    if (!is_open() || timeout_ms <= 0) {
        throw std::invalid_argument("send timeout requires an open socket and positive timeout");
    }
    const DWORD value = static_cast<DWORD>(timeout_ms);
    if (::setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value)) == SOCKET_ERROR) {
        detail::throw_socket_error("setsockopt(SO_SNDTIMEO)", WSAGetLastError());
    }
}

std::uint16_t TcpSocket::local_port() const {
    if (!is_open()) {
        throw std::logic_error("socket is closed");
    }
    sockaddr_in local{};
    int size = sizeof(local);
    if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&local), &size) == SOCKET_ERROR) {
        detail::throw_socket_error("getsockname", WSAGetLastError());
    }
    return ntohs(local.sin_port);
}

bool TcpSocket::is_open() const {
    return handle_ != INVALID_SOCKET;
}

void TcpSocket::close() {
    if (is_open()) {
        ::closesocket(handle_);
        handle_ = INVALID_SOCKET;
    }
}

}  // namespace rtsp::net
