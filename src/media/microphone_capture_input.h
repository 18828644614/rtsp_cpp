#pragma once

#include "media/microphone_audio_source.h"
#include "media/microphone_capture.h"

namespace rtsp {

std::unique_ptr<AudioFrameInput> make_microphone_capture_input(const MicrophoneConfig& config);

}
