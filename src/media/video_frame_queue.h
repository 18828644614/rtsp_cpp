#pragma once

#include "media/h264_access_unit.h"

#include <cstddef>
#include <deque>
#include <exception>
#include <mutex>

namespace rtsp {

struct VideoFrameQueueLimits {
    std::size_t max_frames = 60;
    std::size_t max_bytes = 8 * 1024 * 1024;
};

enum class VideoFrameRead { FrameReady, Waiting, Ended };

struct VideoFrameQueueStats {
    std::size_t frames = 0;
    std::size_t bytes = 0;
    std::size_t dropped = 0;
    bool waiting_for_idr = true;
    bool closed = false;
};

// 单生产者/单消费者；所有状态使用同一把锁。只存放完整、独立拥有内存的 AU。
class VideoFrameQueue {
public:
    explicit VideoFrameQueue(VideoFrameQueueLimits limits = {});
    bool push(H264AccessUnit frame);
    // 无等待；关闭后先排空，再返回 Ended 或重抛生产者异常。
    VideoFrameRead read(H264AccessUnit& frame);
    void finish(std::exception_ptr error = nullptr);
    void discard_and_finish();
    // 重新有人观看时，仅保留最新 IDR 及其后继；没有 IDR 则等待下一个。
    void resume_at_latest_idr();
    VideoFrameQueueStats stats() const;

private:
    void clear_locked();
    const VideoFrameQueueLimits limits_;
    mutable std::mutex mutex_;
    std::deque<H264AccessUnit> frames_;
    std::size_t bytes_ = 0;
    std::size_t dropped_ = 0;
    bool waiting_for_idr_ = true;
    bool closed_ = false;
    std::exception_ptr error_;
};

}  // namespace rtsp
