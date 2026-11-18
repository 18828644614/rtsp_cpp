#pragma once

#include "media/aac_access_unit.h"

#include <cstddef>
#include <deque>
#include <exception>
#include <mutex>

namespace rtsp {

struct AudioFrameQueueLimits {
    std::size_t max_frames = 32;
    std::size_t max_bytes = 128 * 1024;
};

enum class AudioFrameRead { FrameReady, Waiting, Ended };

struct AudioFrameQueueStats {
    std::size_t frames = 0;
    std::size_t bytes = 0;
    std::size_t dropped = 0;
    bool closed = false;
};

class AudioFrameQueue {
public:
    explicit AudioFrameQueue(AudioFrameQueueLimits limits = {});
    bool push(AacAccessUnit frame);
    AudioFrameRead read(AacAccessUnit& frame);
    void finish(std::exception_ptr error = nullptr);
    void discard_and_finish();
    void resume_at_latest();
    AudioFrameQueueStats stats() const;

private:
    void discard_locked();
    const AudioFrameQueueLimits limits_;
    mutable std::mutex mutex_;
    std::deque<AacAccessUnit> frames_;
    std::size_t bytes_ = 0;
    std::size_t dropped_ = 0;
    bool closed_ = false;
    std::exception_ptr error_;
};

}
