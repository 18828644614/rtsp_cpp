#include "media/camera_video_source.h"

#include <stdexcept>
#include <utility>

namespace rtsp {

// CameraVideoSource
// ├── input_：图像输入
// ├── config_：运行配置
// └── queue_：编码帧队列
// input	原始图像的输入对象，负责提供 CameraFrame
// config	编码参数、队列容量、启动超时、读取超时等配置
CameraVideoSource::CameraVideoSource(std::unique_ptr<CameraFrameInput> input,
                                     CameraVideoSourceConfig config)
    : input_(std::move(input)), config_(config), queue_(config.queue) {
    if (!input_ || config.startup_timeout.count() <= 0 || config.read_timeout.count() <= 0) {
        throw std::invalid_argument("camera source requires input and positive timeouts");
    }
    worker_ = std::thread(&CameraVideoSource::produce, this);
    try {
        std::unique_lock<std::mutex> lock(startup_mutex_);
        if (!startup_wake_.wait_for(lock, config_.startup_timeout,
                                   [this] { return ready_ || producer_done_; })) {
            throw std::runtime_error("camera startup timed out waiting for IDR/SPS/PPS");
        }
        if (!ready_) {
            if (startup_error_) { std::rethrow_exception(startup_error_); }
            throw std::runtime_error("camera ended before IDR/SPS/PPS became available");
        }
    } catch (...) {
        stop();
        throw;
    }
}

CameraVideoSource::~CameraVideoSource() { stop(); }

void CameraVideoSource::publish(std::vector<H264AccessUnit> frames, const H264Encoder& encoder) {
    for (auto& frame : frames) {
        if (stopping_.load()) { return; }
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
        encoder.open(config_.encoder);
        while (!stopping_.load()) {
            // 准备一个原始图像容器，向输入对象请求下一张图像，并取得读取状态
            CameraFrame frame;
            const auto result = input_->read(frame, config_.read_timeout);

            if (stopping_.load()) { break; }
            if (result == CameraInputResult::Waiting) { continue; }
            if (result == CameraInputResult::Ended) {
                publish(encoder.flush(), encoder);
                break;
            }
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
    if (result == VideoFrameRead::Ended) { ended_ = true; }
    return result == VideoFrameRead::FrameReady;
}

bool CameraVideoSource::ended() const { return ended_; }
const MediaTrack& CameraVideoSource::track() const { return track_; }
void CameraVideoSource::resume() { queue_.resume_at_latest_idr(); }
VideoFrameQueueStats CameraVideoSource::queue_stats() const { return queue_.stats(); }

void CameraVideoSource::stop() {
    stopping_.store(true);
    input_->request_stop();
    if (worker_.joinable()) { worker_.join(); }
    queue_.discard_and_finish();
    ended_ = true;
}

}  // namespace rtsp
