include_guard(GLOBAL)

set(FFMPEG_ROOT "" CACHE PATH "Root of the FFmpeg shared development package (include/lib/bin)")
find_path(FFMPEG_INCLUDE_DIR libavutil/avutil.h
    HINTS "${FFMPEG_ROOT}" ENV FFMPEG_ROOT PATH_SUFFIXES include)
if(NOT FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "FFmpeg development headers missing. Set -DFFMPEG_ROOT=<shared SDK root>.")
endif()
get_filename_component(ffmpeg_sdk_root "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
set(FFMPEG_RUNTIME_DIR "${ffmpeg_sdk_root}/bin" CACHE PATH "Directory containing FFmpeg runtime DLLs")
file(GLOB ffmpeg_runtime_dlls CONFIGURE_DEPENDS "${FFMPEG_RUNTIME_DIR}/*.dll")
if(NOT ffmpeg_runtime_dlls)
    message(FATAL_ERROR "FFmpeg DLLs missing. Set -DFFMPEG_RUNTIME_DIR=<shared SDK bin>.")
endif()

function(require_ffmpeg)
    foreach(component IN LISTS ARGN)
        if(NOT EXISTS "${FFMPEG_INCLUDE_DIR}/lib${component}/${component}.h")
            message(FATAL_ERROR "FFmpeg ${component} development headers missing.")
        endif()
        if(NOT TARGET FFmpeg::${component})
            string(TOUPPER "${component}" component_upper)
            find_library(FFMPEG_${component_upper}_LIBRARY NAMES "${component}"
                HINTS "${FFMPEG_ROOT}" ENV FFMPEG_ROOT PATH_SUFFIXES lib)
            if(NOT FFMPEG_${component_upper}_LIBRARY)
                message(FATAL_ERROR "FFmpeg ${component} import library missing. Set -DFFMPEG_ROOT=<shared SDK root>.")
            endif()
            add_library(FFmpeg::${component} UNKNOWN IMPORTED GLOBAL)
            set_target_properties(FFmpeg::${component} PROPERTIES
                IMPORTED_LOCATION "${FFMPEG_${component_upper}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
        endif()
    endforeach()
endfunction()

function(copy_ffmpeg_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${ffmpeg_runtime_dlls} "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS VERBATIM)
endfunction()
