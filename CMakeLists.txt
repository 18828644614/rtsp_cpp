cmake_minimum_required(VERSION 3.16)
project(rtsp_cpp VERSION 0.1.0 LANGUAGES CXX)

# 当前先实现 Windows 阻塞式 TCP，后续在 net 模块内扩展其他平台。
if(NOT WIN32)
    message(FATAL_ERROR "This version currently supports Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 客户端和服务器共用网络层，后续逐步向此库添加协议模块。
add_library(rtsp_core STATIC
    src/net/socket_utils.cpp
    src/net/tcp_socket.cpp
    src/rtsp/rtsp_message.cpp
    src/rtsp/rtsp_stream_parser.cpp
)
target_include_directories(rtsp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(rtsp_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0600)
target_link_libraries(rtsp_core PUBLIC ws2_32)

add_executable(rtsp_client src/app/client_main.cpp)
target_link_libraries(rtsp_client PRIVATE rtsp_core)

add_executable(rtsp_server src/app/server_main.cpp)
target_link_libraries(rtsp_server PRIVATE rtsp_core)

# 使用 CTest 注册测试，无需安装第三方测试框架。
include(CTest)
if(BUILD_TESTING)
    add_executable(tcp_tests tests/tcp_tests.cpp)
    target_link_libraries(tcp_tests PRIVATE rtsp_core)
    add_test(NAME tcp_tests COMMAND tcp_tests)
    set_tests_properties(tcp_tests PROPERTIES TIMEOUT 20)

    add_executable(rtsp_tests tests/rtsp_tests.cpp)
    target_link_libraries(rtsp_tests PRIVATE rtsp_core)
    add_test(NAME rtsp_tests COMMAND rtsp_tests)
    set_tests_properties(rtsp_tests PROPERTIES TIMEOUT 20)
endif()

# /utf-8 使 MSVC 正确读取中文注释；两种编译器均开启常用警告。
if(MSVC)
    target_compile_options(rtsp_core PUBLIC /W4 /utf-8)
else()
    target_compile_options(rtsp_core PUBLIC -Wall -Wextra -Wpedantic)
endif()
