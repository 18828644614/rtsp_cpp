#pragma once

// 必须先于 windows.h 包含 winsock2.h，避免旧版 Winsock 声明冲突。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

namespace rtsp::net::detail {

// 第一次使用时初始化 Winsock，程序退出时统一清理。
void initialize_sockets();

// 错误码由调用方立即取得，避免被后续 Socket 调用覆盖。
[[noreturn]] void throw_socket_error(const char* operation, int error_code);

}  // namespace rtsp::net::detail
