#pragma once

#include "media/camera_frame.h"
#include "media/h264_encoder.h"
#include "media/video_frame_queue.h"
#include "media/video_source.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace rtsp {

enum class CameraInputResult { FrameReady, Waiting, Ended };

// 工作线程调用 read/close，拥有者线程可并发 request_stop。
// 实现必须遵守读取期限，request_stop 必须唤醒等待；不可恢复错误用异常报告。
class CameraFrameInput {
public:
    virtual ~CameraFrameInput() = default;
    virtual CameraInputResult read(CameraFrame& frame, std::chrono::milliseconds timeout) = 0;
    virtual void request_stop() noexcept = 0;
    virtual void close() noexcept {}
};

struct CameraVideoSourceConfig {
    H264EncoderConfig encoder;
    VideoFrameQueueLimits queue;
    std::chrono::milliseconds startup_timeout{5000};
    std::chrono::milliseconds read_timeout{100};
};

class CameraVideoSource final : public VideoSource {
public:
    // 输入已配置。限时等待首个可入队的 IDR/SPS/PPS，失败时停止并回收线程。
    CameraVideoSource(std::unique_ptr<CameraFrameInput> input, CameraVideoSourceConfig config = {});
    ~CameraVideoSource() override;
    CameraVideoSource(const CameraVideoSource&) = delete;
    CameraVideoSource& operator=(const CameraVideoSource&) = delete;

    bool read(H264AccessUnit& frame) override;
    bool ended() const override;
    const MediaTrack& track() const override;
    bool is_live() const override { return true; }
    void resume() override;
    void stop() override;
    VideoFrameQueueStats queue_stats() const;

private:
    // 它在工作线程里不断读取原始图像，编码成 H.264，再交给队列，供 MediaSession 读取。
    void produce() noexcept;
    // 把编码器产生的一批 H.264 访问单元逐个放入线程安全队列，并在第一次获得可用的 IDR、SPS、PPS 后，宣布视频源启动完成。
    void publish(std::vector<H264AccessUnit> frames, const H264Encoder& encoder);
    std::unique_ptr<CameraFrameInput> input_;
    const CameraVideoSourceConfig config_;
    VideoFrameQueue queue_;
    MediaTrack track_;
    std::atomic<bool> stopping_{false};
    // ended_ 只由消费者读取队列结果后更新，避免 read(false)/ended() 之间生产者结束的竞态。
    bool ended_ = false;
    std::mutex startup_mutex_;
    std::condition_variable startup_wake_;
    bool ready_ = false;
    bool producer_done_ = false;
    std::exception_ptr startup_error_;
    std::thread worker_;
};

}  // namespace rtsp
