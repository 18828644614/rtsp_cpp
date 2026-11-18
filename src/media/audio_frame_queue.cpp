#include "media/audio_frame_queue.h"

#include <stdexcept>
#include <utility>

namespace rtsp {

AudioFrameQueue::AudioFrameQueue(AudioFrameQueueLimits limits) : limits_(limits) {
    if (!limits.max_frames || !limits.max_bytes) {
        throw std::invalid_argument("audio queue limits must be positive");
    }
}

void AudioFrameQueue::discard_locked() {
    bytes_ -= frames_.front().data.size();
    frames_.pop_front();
    ++dropped_;
}

bool AudioFrameQueue::push(AacAccessUnit frame) {
    if (frame.data.empty() || frame.duration == 0 || frame.data.size() > limits_.max_bytes) {
        throw std::invalid_argument("invalid AAC frame or audio queue byte limit too small");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return false;
    }
    while (frames_.size() >= limits_.max_frames || frame.data.size() > limits_.max_bytes - bytes_) {
        discard_locked();
    }
    frames_.push_back(std::move(frame));
    bytes_ += frames_.back().data.size();
    return true;
}

AudioFrameRead AudioFrameQueue::read(AacAccessUnit& frame) {
    frame = {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frames_.empty()) {
        bytes_ -= frames_.front().data.size();
        frame = std::move(frames_.front());
        frames_.pop_front();
        return AudioFrameRead::FrameReady;
    }
    if (closed_ && error_) {
        std::rethrow_exception(error_);
    }
    return closed_ ? AudioFrameRead::Ended : AudioFrameRead::Waiting;
}

void AudioFrameQueue::finish(std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return;
    }
    error_ = std::move(error);
    closed_ = true;
}

void AudioFrameQueue::discard_and_finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!frames_.empty()) {
        discard_locked();
    }
    error_ = nullptr;
    closed_ = true;
}

void AudioFrameQueue::resume_at_latest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return;
    }
    while (frames_.size() > 1) {
        discard_locked();
    }
}

AudioFrameQueueStats AudioFrameQueue::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {frames_.size(), bytes_, dropped_, closed_};
}

}
