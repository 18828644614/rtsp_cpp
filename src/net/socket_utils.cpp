#include "net/socket_utils.h"

#include <stdexcept>
#include <string>

namespace rtsp::net::detail {
namespace {

// 将 WSAStartup / WSACleanup 配成一对，不让程序入口关心平台细节。
class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error != 0) {
            // WSAStartup 直接返回错误码，不使用 WSAGetLastError。
            throw_socket_error("WSAStartup", error);
        }
    }

    ~WinsockRuntime() {
        WSACleanup();
    }

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

}  // namespace

void initialize_sockets() {
    // C++11 起，函数内 static 的初始化由语言保证线程安全。
    static WinsockRuntime runtime;
}

void throw_socket_error(const char* operation, int error_code) {
    // 显示操作名和 Windows 错误码，保持错误处理直观。
    throw std::runtime_error(std::string(operation) + " failed, error code: "
                             + std::to_string(error_code));
}

}  // namespace rtsp::net::detail
