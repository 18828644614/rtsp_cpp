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
    src/net/event_loop.cpp
    src/net/outbound_queue.cpp
    src/rtsp/rtsp_message.cpp
    src/rtsp/rtsp_stream_parser.cpp
    src/rtsp/rtsp_session_utils.cpp
    src/rtsp/rtsp_server_session.cpp
    src/rtsp/rtsp_server.cpp
    src/rtsp/rtsp_server_connection.cpp
    src/rtsp/media_transport.cpp
    src/rtsp/rtsp_client.cpp
    src/sdp/session_description.cpp
    src/rtp/rtp_packet.cpp
    src/rtp/rtp_sender.cpp
    src/media/h264_packetizer.cpp
    src/media/h264_depacketizer.cpp
    src/media/h264_file_reader.cpp
    src/media/aac_file_reader.cpp
    src/media/aac_packetizer.cpp
    src/media/media_source.cpp
    src/media/media_session.cpp
    src/media/h264_file_writer.cpp
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

    add_executable(event_loop_tests tests/event_loop_tests.cpp)
    target_link_libraries(event_loop_tests PRIVATE rtsp_core)
    add_test(NAME event_loop_tests COMMAND event_loop_tests)
    set_tests_properties(event_loop_tests PROPERTIES TIMEOUT 25)

    add_executable(rtsp_tests tests/rtsp_tests.cpp)
    target_link_libraries(rtsp_tests PRIVATE rtsp_core)
    add_test(NAME rtsp_tests COMMAND rtsp_tests)
    set_tests_properties(rtsp_tests PROPERTIES TIMEOUT 20)

    add_executable(session_tests tests/session_tests.cpp)
    target_link_libraries(session_tests PRIVATE rtsp_core)
    add_test(NAME session_tests COMMAND session_tests)
    set_tests_properties(session_tests PROPERTIES TIMEOUT 20)

    add_executable(rtp_tests tests/rtp_tests.cpp)
    target_link_libraries(rtp_tests PRIVATE rtsp_core)
    add_test(NAME rtp_tests COMMAND rtp_tests)
    set_tests_properties(rtp_tests PROPERTIES TIMEOUT 20)

    add_executable(outbound_tests tests/outbound_tests.cpp)
    target_link_libraries(outbound_tests PRIVATE rtsp_core)
    add_test(NAME outbound_tests COMMAND outbound_tests)
    set_tests_properties(outbound_tests PROPERTIES TIMEOUT 20)

    add_executable(h264_tests tests/h264_tests.cpp)
    target_link_libraries(h264_tests PRIVATE rtsp_core)
    add_test(NAME h264_tests COMMAND h264_tests)
    set_tests_properties(h264_tests PROPERTIES TIMEOUT 20)

    add_executable(media_file_tests tests/media_file_tests.cpp)
    target_link_libraries(media_file_tests PRIVATE rtsp_core)
    add_test(NAME media_file_tests COMMAND media_file_tests)
    set_tests_properties(media_file_tests PROPERTIES TIMEOUT 20)

    add_executable(media_session_tests tests/media_session_tests.cpp)
    target_link_libraries(media_session_tests PRIVATE rtsp_core)
    add_test(NAME media_session_tests COMMAND media_session_tests)
    set_tests_properties(media_session_tests PROPERTIES TIMEOUT 20)

    add_executable(aac_tests tests/aac_tests.cpp)
    target_link_libraries(aac_tests PRIVATE rtsp_core)
    add_test(NAME aac_tests COMMAND aac_tests)
    set_tests_properties(aac_tests PROPERTIES TIMEOUT 20)

    add_executable(multitrack_tests tests/multitrack_tests.cpp)
    target_link_libraries(multitrack_tests PRIVATE rtsp_core)
    add_test(NAME multitrack_tests COMMAND multitrack_tests)
    set_tests_properties(multitrack_tests PROPERTIES TIMEOUT 20)

    find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if(POWERSHELL_EXECUTABLE)
        add_test(NAME media_integration COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/media_integration.ps1"
            -ServerPath "$<TARGET_FILE:rtsp_server>"
            -ClientPath "$<TARGET_FILE:rtsp_client>"
            -WorkDirectory "${CMAKE_CURRENT_BINARY_DIR}/media_integration")
        set_tests_properties(media_integration PROPERTIES TIMEOUT 45)
    endif()
endif()

# /utf-8 使 MSVC 正确读取中文注释；两种编译器均开启常用警告。
if(MSVC)
    target_compile_options(rtsp_core PUBLIC /W4 /utf-8)
else()
    target_compile_options(rtsp_core PUBLIC -Wall -Wextra -Wpedantic)
endif()
