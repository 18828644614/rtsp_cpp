#pragma once

#include <memory>
#include <string>

struct AVFrame;

namespace rtsp {

class AudioResampler {
public:
    AudioResampler();
    ~AudioResampler();
    AudioResampler(const AudioResampler&) = delete;
    AudioResampler& operator=(const AudioResampler&) = delete;

    void open(int sample_rate, const std::string& sample_format);
    std::shared_ptr<AVFrame> convert(const AVFrame& input);
    std::shared_ptr<AVFrame> flush();
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
