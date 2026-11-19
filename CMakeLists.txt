cmake_minimum_required(VERSION 3.16)
project(rtsp_cpp VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The live capture server requires Windows and DirectShow.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/FFmpeg.cmake)
require_ffmpeg(avdevice avformat avcodec avutil swscale swresample)

# 一个库供服务器和测试共用，摄像头与麦克风始终一起构建。
add_library(rtsp_core STATIC
    src/media/camera_capture.cpp
    src/media/camera_capture_input.cpp
    src/media/camera_video_source.cpp
    src/media/h264_encoder.cpp
    src/media/video_frame_queue.cpp
    src/media/microphone_capture.cpp
    src/media/microphone_capture_input.cpp
    src/media/microphone_audio_source.cpp
    src/media/audio_resampler.cpp
    src/media/aac_encoder.cpp
    src/media/audio_frame_queue.cpp
    src/media/h264_packetizer.cpp
    src/media/aac_packetizer.cpp
    src/media/media_clock.cpp
    src/media/media_session.cpp
    src/net/socket_utils.cpp
    src/net/tcp_socket.cpp
    src/net/event_loop.cpp
    src/net/outbound_queue.cpp
    src/rtsp/rtsp_message.cpp
    src/rtsp/rtsp_stream_parser.cpp
    src/rtsp/rtsp_session_utils.cpp
    src/rtsp/rtsp_server_session.cpp
    src/rtsp/rtsp_server_connection.cpp
    src/rtsp/rtsp_server.cpp
    src/sdp/session_description.cpp
    src/rtp/rtp_packet.cpp
    src/rtp/rtp_sender.cpp
    src/rtp/rtcp_packet.cpp)
target_include_directories(rtsp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(rtsp_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0600)
target_link_libraries(rtsp_core PUBLIC ws2_32
    PRIVATE FFmpeg::avdevice FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil
            FFmpeg::swscale FFmpeg::swresample ole32 oleaut32 strmiids)
if(MSVC)
    target_compile_options(rtsp_core PUBLIC /W4 /utf-8)
else()
    target_compile_options(rtsp_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

add_executable(rtsp_server src/app/server_main.cpp)
target_link_libraries(rtsp_server PRIVATE rtsp_core)
if(MINGW)
    target_link_options(rtsp_server PRIVATE -municode)
endif()
copy_ffmpeg_runtime(rtsp_server)

include(CTest)
if(BUILD_TESTING)
    add_library(test_receivers STATIC tests/support/h264_depacketizer.cpp)
    target_link_libraries(test_receivers PUBLIC rtsp_core)

    foreach(test_name IN ITEMS
        server_options_tests tcp_tests event_loop_tests outbound_tests
        rtsp_tests session_tests rtp_tests runtime_tests
        h264_tests aac_tests media_session_tests
        audio_frame_queue_tests video_frame_queue_tests
        camera_capture_tests microphone_capture_tests
        h264_encoder_tests aac_encoder_tests
        camera_video_source_tests microphone_audio_source_tests camera_rtsp_tests)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE rtsp_core)
        # 测试与服务器在同一输出目录，FFmpeg DLL 只复制一次。
        add_dependencies(${test_name} rtsp_server)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
    endforeach()
    foreach(test_name IN ITEMS camera_capture_tests microphone_capture_tests
        h264_encoder_tests aac_encoder_tests camera_video_source_tests
        microphone_audio_source_tests camera_rtsp_tests)
        target_link_libraries(${test_name} PRIVATE FFmpeg::avcodec FFmpeg::avutil)
    endforeach()
    target_link_libraries(camera_rtsp_tests PRIVATE test_receivers)
    target_link_libraries(h264_tests PRIVATE test_receivers)

    add_test(NAME rtsp_server_help COMMAND rtsp_server --help)
    add_test(NAME rtsp_server_requires_both COMMAND rtsp_server --camera 0)
    set_tests_properties(rtsp_server_requires_both PROPERTIES WILL_FAIL TRUE)

    # 可选：用外部播放器的解码器验证真实 TCP 双轨流，不打开物理设备。
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(FFMPEG_EXECUTABLE NAMES ffmpeg HINTS "${FFMPEG_RUNTIME_DIR}")
    if(Python3_Interpreter_FOUND AND FFMPEG_EXECUTABLE)
        add_test(NAME live_av_integration COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_camera_rtsp.py"
            --fixture "$<TARGET_FILE:camera_rtsp_tests>" --ffmpeg "${FFMPEG_EXECUTABLE}"
            --output "${CMAKE_CURRENT_BINARY_DIR}/live_av_integration")
        set_tests_properties(live_av_integration PROPERTIES TIMEOUT 30)
    endif()
endif()
