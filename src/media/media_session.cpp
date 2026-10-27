#include "media/media_session.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace rtsp {

MediaSubscription::MediaSubscription(std::shared_ptr<net::OutboundQueue> queue)
    : queue_(std::move(queue)) {
    if (!queue_) {
        throw std::invalid_argument("subscription requires an outbound queue");
    }
}

void MediaSubscription::add_track(std::size_t index, const MediaTrack& track,
                                   TcpTransport transport, std::uint16_t sequence,
                                   std::uint32_t ssrc) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ || started_) {
        throw std::logic_error("add tracks before PLAY");
    }
    for (const TrackSender& existing : tracks_) {
        if (existing.index == index) {
            throw std::logic_error("track already added");
        }
    }
    TrackSender state;
    state.index = index;
    state.sender = std::make_unique<RtpSender>(MediaTransport(transport),
                                             track.payload_type, sequence, ssrc);
    tracks_.push_back(std::move(state));
    has_video_ = has_video_ || track.media_type == "video";
}

MediaSubscription::~MediaSubscription() {
    cancel();
}

bool MediaSubscription::start(const std::string& play_response) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tracks_.empty() || !queue_->push({net::OutboundKind::RtspResponse, play_response})) {
        return false;
    }
    active_ = true;
    return true;
}

void MediaSubscription::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
}

bool MediaSubscription::finished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !active_;
}

bool MediaSubscription::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

void MediaSubscription::deliver(std::size_t track_index, const RtpPayloadUnit& unit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return;
    }
    for (TrackSender& track : tracks_) {
        if (track.index != track_index) {
            continue;
        }
        if (!started_) {
            // 有视频时等待 IDR；只订阅音频时从当前 AU 开始。
            if (has_video_ && !unit.is_keyframe) {
                return;
            }
            start_pts_ = unit.pts;
            start_clock_rate_ = unit.clock_rate;
            started_ = true;
        }
        if (unit.pts * start_clock_rate_ < start_pts_ * unit.clock_rate) {
            return;
        }
        const std::uint64_t base = start_pts_ * unit.clock_rate / start_clock_rate_;
        const auto timestamp = static_cast<std::uint32_t>(unit.pts - base);
        try {
            if (track.sender->send_payloads(unit, timestamp, *queue_)) {
                return;
            }
        } catch (const std::exception& error) {
            std::cerr << "Subscription send error: " << error.what() << '\n';
        }
        failed_ = true;
        active_ = false;
        queue_->close();
        return;
    }
}

void MediaSubscription::finish(bool failed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return;  // 已取消的订阅由连接自行关闭队列，不能抢在 TEARDOWN 响应前关闭。
    }
    failed_ = failed_ || failed;
    if (!started_ && has_video_ && !failed) {
        std::cout << "Subscription ended before an IDR became available" << std::endl;
    }
    active_ = false;
    // 正常 EOF 允许事件循环排空队列；异常由连接处理路径中断。
    queue_->close();
}

MediaSession::MediaSession(const std::string& video_path, const std::string& audio_path) {
    description_.tracks.clear();
    if (!video_path.empty()) {
        video_source_ = std::make_unique<H264MediaSource>(video_path);
        video_track_ = description_.tracks.size();
        description_.tracks.push_back(video_source_->description().tracks[0]);
    }
    if (!audio_path.empty()) {
        audio_source_ = std::make_unique<AacFileReader>();
        audio_source_->open(audio_path);
        audio_track_ = description_.tracks.size();
        description_.tracks.push_back(audio_source_->track());
    }
    if (description_.tracks.empty()) {
        throw std::invalid_argument("provide a video or audio file");
    }
}

MediaSession::~MediaSession() {
    stop();
}

const SessionDescription& MediaSession::description() const {
    return description_;
}

SubscribeResult MediaSession::subscribe(const std::shared_ptr<MediaSubscription>& subscription,
                                        const std::string& play_response, bool from_start) {
    if (!subscription) {
        throw std::invalid_argument("expected a media subscription");
    }
    if (ended_) {
        return SubscribeResult::Ended;
    }
    if (from_start && started_) {
        return SubscribeResult::InvalidRange;
    }
    bool had_active = false;
    for (const auto& weak : subscribers_) {
        if (auto existing = weak.lock()) {
            had_active = had_active || !existing->finished();
        }
    }
    // 先注册，再入队；分配失败时不会先发出成功响应。
    subscribers_.push_back(subscription);
    if (!subscription->start(play_response)) {
        subscribers_.pop_back();
        return SubscribeResult::QueueClosed;
    }

    if (!had_active) {
        reset_clock_ = true;
        next_deadline_ = Clock::now();
    }
    return SubscribeResult::Started;
}

bool MediaSession::ended() const {
    return ended_;
}

void MediaSession::stop() {
    finish(false);
}

std::optional<MediaSession::Clock::time_point> MediaSession::next_deadline() const {
    return next_deadline_;
}

void MediaSession::finish(bool failed) {
    ended_ = true;
    next_deadline_.reset();
    video_.reset();
    audio_.reset();
    std::vector<std::weak_ptr<MediaSubscription>> subscribers;
    subscribers.swap(subscribers_);
    for (const auto& weak : subscribers) {
        if (auto subscriber = weak.lock()) {
            subscriber->finish(failed);
        }
    }
}

void MediaSession::tick(Clock::time_point now) {
    if (ended_) {
        return;
    }
    try {
        // 单轮最多推进 32 个 AU，媒体落后时也要让出网络处理机会。
        // AU 可以理解为一份完整的编码媒体单元：
        // - H.264 通常是一帧访问单元；
        // - AAC 通常是一帧音频访问单元。
        // 设置 32 的目的，是防止媒体落后时，tick() 一直处理媒体而不返回，导致网络事件得不到处理
        for (int budget = 0; budget < 32; ++budget) {
            std::vector<std::shared_ptr<MediaSubscription>> subscribers;
            auto it = subscribers_.begin();
            while (it != subscribers_.end()) {
                auto subscriber = it->lock();
                if (!subscriber || subscriber->finished()) {
                    it = subscribers_.erase(it);
                } else {
                    subscribers.push_back(std::move(subscriber));
                    ++it;
                }
            }
            if (subscribers.empty()) {
                // 表示以后有新订阅加入时，需要重新建立媒体时钟起点
                reset_clock_ = true;
                // 表示当前没有下一帧的发送期限。
                next_deadline_.reset();
                return;
            }
            started_ = true;

            // 每条轨道只预读一帧，选择媒体时间更早的帧。负载只封装一次。
            if (!video_ && video_source_ && !video_source_->ended()) {
                const auto frame = video_source_->read();
                if (frame) {
                    video_ = video_packetizer_.packetize_access_unit(frame);
                }
            }
            if (!audio_ && audio_source_ && !audio_source_->ended()) {
                AacAccessUnit frame;
                if (audio_source_->read(frame)) {
                    audio_ = audio_packetizer_.packetize(frame, audio_source_->track().clock_rate);
                }
            }
            if (!video_ && !audio_) {
                finish(false);
                return;
            }
            bool send_video = false;
            if (video_ && (!audio_ || video_->pts * audio_->clock_rate <= audio_->pts * video_->clock_rate)) {
                send_video = true;
            }
            const auto unit = send_video ? video_ : audio_;
            const std::size_t track_index = send_video ? video_track_ : audio_track_;
            const auto media_time = std::chrono::microseconds(unit->pts * 1000000 / unit->clock_rate);
            // clock_origin 表示：媒体时间0对应的时刻
            if (reset_clock_) {
                clock_origin_ = now - media_time;
                reset_clock_ = false;
            }
            next_deadline_ = clock_origin_ + media_time;
            if (now < *next_deadline_) {
                return;
            }
            for (const auto& subscriber : subscribers) {
                subscriber->deliver(track_index, *unit);
            }
            if (send_video) {
                video_.reset();
            } else {
                audio_.reset();
            }
            // 一条轨道 EOF 不关闭连接；两条轨道均发送完才统一结束。
            if (!video_ && !audio_ && (!video_source_ || video_source_->ended())
                && (!audio_source_ || audio_source_->ended())) {
                finish(false);
                return;
            }
        }
        next_deadline_ = now;
    } catch (const std::exception& error) {
        std::cerr << "MediaSession error: " << error.what() << '\n';
        finish(true);
    }
}

}  // namespace rtsp
