set(FFMPEG_ROOT "" CACHE PATH "Root of the FFmpeg shared development package (include/lib/bin)")
find_path(FFMPEG_INCLUDE_DIR libavdevice/avdevice.h
    HINTS "${FFMPEG_ROOT}" ENV FFMPEG_ROOT PATH_SUFFIXES include)
foreach(component avdevice avformat avcodec avutil)
    string(TOUPPER "${component}" component_upper)
    find_library(FFMPEG_${component_upper}_LIBRARY NAMES "${component}"
        HINTS "${FFMPEG_ROOT}" ENV FFMPEG_ROOT PATH_SUFFIXES lib)
    if(NOT FFMPEG_${component_upper}_LIBRARY)
        message(FATAL_ERROR "FFmpeg ${component} import library missing. Set -DFFMPEG_ROOT=<shared SDK root>.")
    endif()
endforeach()
if(NOT FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "FFmpeg development headers missing. Set -DFFMPEG_ROOT=<shared SDK root>.")
endif()
get_filename_component(ffmpeg_sdk_root "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
set(FFMPEG_RUNTIME_DIR "${ffmpeg_sdk_root}/bin" CACHE PATH "Directory containing FFmpeg runtime DLLs")
file(GLOB ffmpeg_runtime_dlls CONFIGURE_DEPENDS "${FFMPEG_RUNTIME_DIR}/*.dll")
if(NOT ffmpeg_runtime_dlls)
    message(FATAL_ERROR "FFmpeg DLLs missing. Set -DFFMPEG_RUNTIME_DIR=<shared SDK bin>.")
endif()

add_library(camera_capture STATIC src/media/camera_capture.cpp)
target_include_directories(camera_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(camera_capture SYSTEM PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_compile_definitions(camera_capture PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0600)
target_link_libraries(camera_capture PRIVATE
    "${FFMPEG_AVDEVICE_LIBRARY}" "${FFMPEG_AVFORMAT_LIBRARY}"
    "${FFMPEG_AVCODEC_LIBRARY}" "${FFMPEG_AVUTIL_LIBRARY}"
    ole32 oleaut32 strmiids)
if(MSVC)
    target_compile_options(camera_capture PUBLIC /W4 /utf-8)
else()
    target_compile_options(camera_capture PUBLIC -Wall -Wextra -Wpedantic)
endif()

function(copy_camera_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${ffmpeg_runtime_dlls} "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS VERBATIM)
endfunction()

add_executable(camera_probe src/app/camera_probe_main.cpp)
target_link_libraries(camera_probe PRIVATE camera_capture)
if(MINGW)
    target_link_options(camera_probe PRIVATE -municode)
endif()
copy_camera_runtime(camera_probe)

if(BUILD_TESTING)
    add_executable(camera_capture_tests tests/camera_capture_tests.cpp)
    target_include_directories(camera_capture_tests SYSTEM PRIVATE "${FFMPEG_INCLUDE_DIR}")
    target_link_libraries(camera_capture_tests PRIVATE camera_capture "${FFMPEG_AVUTIL_LIBRARY}")
    copy_camera_runtime(camera_capture_tests)
    add_test(NAME camera_capture_tests COMMAND camera_capture_tests)
    set_tests_properties(camera_capture_tests PROPERTIES TIMEOUT 15)
    add_test(NAME camera_probe_help COMMAND camera_probe --help)
    add_test(NAME camera_probe_invalid_option COMMAND camera_probe --not-an-option)
    set_tests_properties(camera_probe_help camera_probe_invalid_option PROPERTIES TIMEOUT 10)
    set_tests_properties(camera_probe_invalid_option PROPERTIES WILL_FAIL TRUE)
endif()
