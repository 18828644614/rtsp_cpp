#include "media/camera_video_source.h"
#include "media/media_time.h"

extern "C" {
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace rtsp {
namespace {

// 只用首帧到达时刻估计偏移，有设备 PTS 时不把后续到达抖动写进时间线。
class CameraTimeMapper {
public:
    std::int64_t map(const CameraFrame& frame, const CaptureTimeline& timeline) {
        if (!start_us_) {
            start_us_ = timeline.relative_us(frame.arrival_steady_us);
            source_origin_ = frame.source_pts;
            time_base_ = {frame.time_base_num, frame.time_base_den};
        }
        if (!source_origin_) {
            return timeline.relative_us(frame.arrival_steady_us);
        }
        if (!frame.source_pts || time_base_.num <= 0 || time_base_.den <= 0 ||
            time_base_.num != frame.time_base_num || time_base_.den != frame.time_base_den) {
            throw std::runtime_error("camera device timestamp missing or time base changed");
        }
        const auto pts = *frame.source_pts;
        const auto origin = *source_origin_;
        if (pts < origin || (origin < 0 && pts > std::numeric_limits<std::int64_t>::max() + origin)) {
            throw std::runtime_error("camera device timestamp moved backwards or overflowed");
        }
        const auto elapsed = av_rescale_q(pts - origin, time_base_, {1, 1000000});
        if (elapsed < 0) {
            throw std::overflow_error("camera device timestamp out of range");
        }
        return add_media_pts(*start_us_, elapsed);
    }

private:
    std::optional<std::int64_t> start_us_;
    std::optional<std::int64_t> source_origin_;
    AVRational time_base_{};
};

}  // namespace

// CameraVideoSource
// ├── input_：图像输入
// ├── config_：运行配置
// └── queue_：编码帧队列
// input	原始图像的输入对象，负责提供 CameraFrame
// config	编码参数、队列容量、启动超时、读取超时等配置
CameraVideoSource::CameraVideoSource(std::unique_ptr<CameraFrameInput> input,
                                     CameraVideoSourceConfig config,
                                     const std::atomic<bool>* stop_requested)
    : input_(std::move(input)), config_(config), queue_(config.queue) {
    if (!input_ || config.startup_timeout.count() <= 0 || config.read_timeout.count() <= 0 ||
        config.no_data_timeout.count() <= 0) {
        throw std::invalid_argument("camera source requires input and positive timeouts");
    }
    worker_ = std::thread(&CameraVideoSource::produce, this);
    try {
        std::unique_lock<std::mutex> lock(startup_mutex_);
        const auto deadline = std::chrono::steady_clock::now() + config_.startup_timeout;
        while (!ready_ && !producer_done_) {
            if (stop_requested && stop_requested->load()) {
                throw std::runtime_error("camera startup stopped");
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                throw std::runtime_error("camera startup timed out waiting for IDR/SPS/PPS");
            }
            // 控制处理线程只置标记；构造线程负责通知输入并回收生产线程。
            startup_wake_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)));
        }
        if (!ready_) {
            if (startup_error_) {
                std::rethrow_exception(startup_error_);
            }
            throw std::runtime_error("camera ended before IDR/SPS/PPS became available");
        }
    } catch (...) {
        stop();
        throw;
    }
}

CameraVideoSource::~CameraVideoSource() {
    stop();
}

void CameraVideoSource::publish(std::vector<H264AccessUnit> frames, const H264Encoder& encoder) {
    for (auto& frame : frames) {
        if (stopping_.load()) {
            return;
        }
        // 是否为关键帧
        const bool idr = frame.is_keyframe;
        if (!queue_.push(std::move(frame))) {
            if (idr && !stopping_.load()) {
                throw std::runtime_error("camera IDR exceeds video queue byte limit");
            }
            continue;
        }
        // track_ 发布一次，此后编码器和生产者不再修改，引用可安全用于 SDP。
        // 至少一个 IDR 已经成功进入队列
        // 并且已经有有效 SPS/PPS
        // 轨道的 SDP fmtp 已经可以生成
        if (!ready_ && idr && !encoder.sps().empty() && !encoder.pps().empty()) {
            std::lock_guard<std::mutex> lock(startup_mutex_);
            track_.fmtp = make_h264_fmtp(encoder.sps(), encoder.pps());
            ready_ = true;
            startup_wake_.notify_all();
        }
    }
}

void CameraVideoSource::produce() noexcept {
    std::exception_ptr error;
    try {
        H264Encoder encoder;
        auto encoder_config = config_.encoder;
        encoder_config.use_capture_timeline = true;
        encoder.open(encoder_config);
        CameraTimeMapper mapper;
        auto last_data = std::chrono::steady_clock::now();
        while (!stopping_.load()) {
            // 准备一个原始图像容器，向输入对象请求下一张图像，并取得读取状态
            CameraFrame frame;
            const auto result = input_->read(frame, config_.read_timeout);

            if (stopping_.load()) {
                break;
            }
            if (result == CameraInputResult::Waiting) {
                if (std::chrono::steady_clock::now() - last_data >= config_.no_data_timeout) {
                    throw std::runtime_error("camera stopped producing frames");
                }
                continue;
            }
            if (result == CameraInputResult::Ended) {
                publish(encoder.flush(), encoder);
                break;
            }
            last_data = std::chrono::steady_clock::now();
            frame.capture_pts_us = mapper.map(frame, config_.timeline);
            publish(encoder.encode(frame), encoder);
        }
    } catch (...) {
        error = std::current_exception();
    }
    input_->close();
    queue_.finish(error);
    {
        std::lock_guard<std::mutex> lock(startup_mutex_);
        startup_error_ = error;
        producer_done_ = true;
    }
    startup_wake_.notify_all();
}

bool CameraVideoSource::read(H264AccessUnit& frame) {
    const auto result = queue_.read(frame);
    if (result == VideoFrameRead::Ended) {
        ended_ = true;
    }
    return result == VideoFrameRead::FrameReady;
}

bool CameraVideoSource::ended() const {
    return ended_;
}

void CameraVideoSource::check_health() const {
    std::lock_guard<std::mutex> lock(startup_mutex_);
    if (producer_done_) {
        if (startup_error_) {
            std::rethrow_exception(startup_error_);
        }
        throw std::runtime_error("camera input ended");
    }
}
const MediaTrack& CameraVideoSource::track() const {
    return track_;
}
void CameraVideoSource::resume() {
    queue_.resume_at_latest_idr();
}
VideoFrameQueueStats CameraVideoSource::queue_stats() const {
    return queue_.stats();
}

void CameraVideoSource::stop() {
    stopping_.store(true);
    input_->request_stop();
    if (worker_.joinable()) {
        worker_.join();
    }
    queue_.discard_and_finish();
    ended_ = true;
}

}  // namespace rtsp
