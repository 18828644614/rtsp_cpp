#pragma once

#include "media/audio_frame.h"
#include "media/aac_access_unit.h"
#include "sdp/session_description.h"

#include <memory>
#include <vector>

namespace rtsp {

struct AacEncoderConfig {
    int bitrate = 64000;
};

struct AacEncoderStats {
    std::int64_t input_samples = 0;
    std::int64_t resampled_samples = 0;
    std::int64_t submitted_samples = 0;
    int tail_padding = 0;
    int initial_padding = 0;
    int trailing_padding = 0;
    std::uint64_t skip_samples = 0;
    std::uint64_t discard_padding = 0;
    std::uint64_t packets = 0;
};

class AacEncoder {
public:
    AacEncoder();
    ~AacEncoder();
    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    void open(const AacEncoderConfig& config = {});
    std::vector<AacAccessUnit> encode(const AudioFrame& input);
    std::vector<AacAccessUnit> flush();
    void close() noexcept;
    const MediaTrack& track() const;
    AacEncoderStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
