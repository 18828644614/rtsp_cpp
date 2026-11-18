#pragma once

#include "media/aac_file_reader.h"
#include "media/audio_source.h"

#include <string>

namespace rtsp {

// 构造时读取并验证文件；之后由 MediaSession::tick 推进游标。
class FileAudioSource final : public AudioSource {
public:
    explicit FileAudioSource(const std::string& path);

    bool read(AacAccessUnit& frame) override;
    bool ended() const override;
    const MediaTrack& track() const override;

private:
    AacFileReader reader_;
};

}  // namespace rtsp
