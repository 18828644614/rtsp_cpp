#pragma once

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <memory>
#include <stdexcept>
#include <string>

namespace rtsp::detail {

inline void check_audio(int result, const char* operation) {
    if (result >= 0) {
        return;
    }
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}

inline std::shared_ptr<AVFrame> audio_buffer(int rate, int channels,
                                            AVSampleFormat format, int samples) {
    auto frame = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* buffer) {
        av_frame_free(&buffer);
    });
    if (!frame) {
        throw std::bad_alloc();
    }
    frame->sample_rate = rate;
    frame->format = format;
    frame->nb_samples = samples;
    av_channel_layout_default(&frame->ch_layout, channels);
    check_audio(av_frame_get_buffer(frame.get(), 0), "allocate audio buffer");
    return frame;
}

}
