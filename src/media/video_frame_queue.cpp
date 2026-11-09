#include "media/video_frame_queue.h"

#include <stdexcept>
#include <utility>

namespace rtsp {
namespace {
std::size_t frame_bytes(const H264AccessUnit& frame) {
    std::size_t bytes = 0;
    for (const auto& nalu : frame.nalus) { bytes += nalu.size(); }
    return bytes;
}
}

VideoFrameQueue::VideoFrameQueue(VideoFrameQueueLimits limits) : limits_(limits) {
    if (limits.max_frames == 0 || limits.max_bytes == 0) {
        throw std::invalid_argument("video queue limits must be positive");
    }
}

void VideoFrameQueue::clear_locked() {
    dropped_ += frames_.size();
    frames_.clear();
    bytes_ = 0;
}

bool VideoFrameQueue::push(H264AccessUnit frame) {
    const auto bytes = frame_bytes(frame);
    if (frame.nalus.empty() || bytes == 0) {
        throw std::invalid_argument("video queue requires a nonempty access unit");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) { return false; }
    if (bytes > limits_.max_bytes || frames_.size() >= limits_.max_frames
        || bytes > limits_.max_bytes - bytes_) {
        // 丢失任何参考帧后，不能把依赖它的 P 帧继续交给解码器。
        clear_locked();
        waiting_for_idr_ = true;
    }
    if (bytes > limits_.max_bytes || (waiting_for_idr_ && !frame.is_keyframe)) {
        ++dropped_;
        return false;
    }
    frames_.push_back(std::move(frame));
    bytes_ += bytes;
    waiting_for_idr_ = false;
    return true;
}

VideoFrameRead VideoFrameQueue::read(H264AccessUnit& frame) {
    frame = {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frames_.empty()) {
        bytes_ -= frame_bytes(frames_.front());
        frame = std::move(frames_.front());
        frames_.pop_front();
        return VideoFrameRead::FrameReady;
    }
    if (closed_ && error_) { std::rethrow_exception(error_); }
    return closed_ ? VideoFrameRead::Ended : VideoFrameRead::Waiting;
}

void VideoFrameQueue::finish(std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) { return; }
    error_ = std::move(error);
    closed_ = true;
}

void VideoFrameQueue::discard_and_finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_locked();
    error_ = nullptr;
    closed_ = true;
}

void VideoFrameQueue::resume_at_latest_idr() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t keep = frames_.size();
    for (std::size_t i = frames_.size(); i > 0; --i) {
        if (frames_[i - 1].is_keyframe) { keep = i - 1; break; }
    }
    for (std::size_t i = 0; i < keep; ++i) {
        bytes_ -= frame_bytes(frames_.front());
        frames_.pop_front();
        ++dropped_;
    }
    waiting_for_idr_ = frames_.empty();
}

VideoFrameQueueStats VideoFrameQueue::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {frames_.size(), bytes_, dropped_, waiting_for_idr_, closed_};
}

}  // namespace rtsp
