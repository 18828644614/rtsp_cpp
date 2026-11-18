#include "media/file_audio_source.h"

namespace rtsp {

FileAudioSource::FileAudioSource(const std::string& path) {
    reader_.open(path);
}

bool FileAudioSource::read(AacAccessUnit& frame) {
    return reader_.read(frame);
}

bool FileAudioSource::ended() const {
    return reader_.ended();
}

const MediaTrack& FileAudioSource::track() const {
    return reader_.track();
}

}  // namespace rtsp
