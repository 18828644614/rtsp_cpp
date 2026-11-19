#pragma once

#include "media/audio_source.h"
#include "media/video_source.h"

namespace test {

// Fixed bytes for protocol tests. Encoding/decoding is covered by the live fixture.
class WireVideo final : public rtsp::VideoSource {
public:
    explicit WireVideo(int frames, std::size_t nalu_size = 2, std::int64_t frame_step = 3600)
        : frames_(frames), nalu_(std::string(1, '\x65') + std::string(nalu_size - 1, 'x')),
          frame_step_(frame_step) {}

    bool read(rtsp::H264AccessUnit& frame) override {
        if (next_ >= frames_) {
            return false; // The live source is idle, not at EOF.
        }
        frame = {{nalu_}, true, next_++ * frame_step_, static_cast<std::uint32_t>(frame_step_)};
        return true;
    }
    bool ended() const override { return false; }
    const rtsp::MediaTrack& track() const override { return track_; }
    void check_health() const override {}
    void resume() override {}
    void stop() override {}

private:
    int frames_;
    int next_ = 0;
    std::string nalu_;
    std::int64_t frame_step_;
    rtsp::MediaTrack track_;
};

class WireAudio final : public rtsp::AudioSource {
public:
    // These connection tests request only the video track; audio may be idle.
    bool read(rtsp::AacAccessUnit&) override { return false; }
    bool ended() const override { return false; }
    const rtsp::MediaTrack& track() const override { return track_; }
    void check_health() const override {}
    void stop() override {}

private:
    rtsp::MediaTrack track_{"audio", "trackID=1", 97, "MPEG4-GENERIC", 48000, 1,
        "streamtype=5;profile-level-id=1;mode=AAC-hbr;config=1188;"
        "SizeLength=13;IndexLength=3;IndexDeltaLength=3"};
};

}  // namespace test
