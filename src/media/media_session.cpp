#include "media/media_session.h"
#include "media/media_time.h"
#include "rtsp/rtsp_stream_parser.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace rtsp {
namespace {
constexpr std::chrono::milliseconds max_live_lateness{50};
constexpr std::chrono::milliseconds input_retry{5};
constexpr std::size_t video_track = 0;
constexpr std::size_t audio_track = 1;
}

MediaSubscription::MediaSubscription(std::shared_ptr<net::OutboundQueue> queue)
    : queue_(std::move(queue)) {
    if (!queue_) {
        throw std::invalid_argument("subscription requires an outbound queue");
    }
}

void MediaSubscription::add_track(std::size_t index, const MediaTrack& track,
                                   TcpTransport transport, std::uint16_t sequence,
                                   std::uint32_t ssrc, std::uint32_t timestamp_origin) {
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
    state.transport = transport;
    state.control = track.control;
    state.clock_rate = track.clock_rate;
    state.timestamp_origin = timestamp_origin;
    state.sender = std::make_unique<RtpSender>(transport.rtp_channel,
                                             track.payload_type, sequence, ssrc);
    tracks_.push_back(std::move(state));
    has_video_ = has_video_ || track.media_type == "video";
}

MediaSubscription::~MediaSubscription() {
    cancel();
}

bool MediaSubscription::start(const std::string& play_response) {
    if (tracks_.empty() || !queue_->push({net::OutboundKind::RtspResponse, play_response})) {
        return false;
    }
    active_ = true;
    return true;
}

void MediaSubscription::cancel() {
    active_ = false;
}

bool MediaSubscription::queue_report(TrackSender& track, bool final) {
    net::OutboundMessage message;
    message.kind = net::OutboundKind::Rtcp;
    message.ssrc = track.sender->ssrc();
    message.final_rtcp = final;
    // 先占据准确的队列字节额度；连接真正发送本项时再填写计数与时间快照。
    message.wire = serialize_interleaved_frame(
        static_cast<unsigned int>(track.transport.rtcp_channel),
        make_rtcp_sender_report(message.ssrc, 0, 0, 0, 0, "rtsp-cpp", final));
    if (!queue_->push(std::move(message))) {
        failed_ = true;
        active_ = false;
        queue_->close();
        return false;
    }
    track.report_pending = true;
    return true;
}

void MediaSubscription::end() {
    active_ = false;
    for (auto& track : tracks_) {
        if (track.queued_rtp && !track.final_queued && !failed_) {
            track.final_queued = queue_report(track, true);
        }
    }
}

std::string MediaSubscription::rtp_info(const std::string& base_url) const {
    std::string info;
    for (const auto& track : tracks_) {
        if (!info.empty()) {
            info += ',';
        }
        info += "url=" + resolve_control_url(base_url, track.control) + ";seq=" + std::to_string(track.sender->next_sequence());
    }
    return info;
}

void MediaSubscription::on_sent(const net::OutboundMessage& message) {
    for (auto& track : tracks_) {
        if (track.sender->ssrc() != message.ssrc) {
            continue;
        }
        if (message.kind == net::OutboundKind::Rtp) {
            ++track.packets;
            track.octets += static_cast<std::uint32_t>(message.rtp_payload_bytes);
        } else if (message.kind == net::OutboundKind::Rtcp) {
            track.report_pending = false;
        }
        return;
    }
}

void MediaSubscription::tick_rtcp(MediaClock::Clock::time_point now,
                                   std::chrono::milliseconds interval) {
    if (!active_ || queue_->closed()) {
        return;
    }
    for (auto& track : tracks_) {
        if (track.packets != 0 && !track.report_pending
            && (!track.next_report || now >= *track.next_report)) {
            if (!queue_report(track, false)) {
                return;
            }
            track.next_report = now + interval;
        }
    }
}

std::optional<MediaClock::Clock::time_point> MediaSubscription::next_rtcp_deadline() const {
    std::optional<MediaClock::Clock::time_point> next;
    if (!active_) {
        return next;
    }
    for (const auto& track : tracks_) {
        if (track.packets == 0 || track.report_pending) {
            continue;
        }
        const auto deadline = track.next_report.value_or(MediaClock::Clock::now());
        if (!next || deadline < *next) {
            next = deadline;
        }
    }
    return next;
}

std::string MediaSubscription::prepare_rtcp(const net::OutboundMessage& message,
                                            const MediaClock& clock,
                                            MediaClock::Clock::time_point now) const {
    for (const auto& track : tracks_) {
        if (track.sender->ssrc() == message.ssrc) {
            return serialize_interleaved_frame(
                static_cast<unsigned int>(track.transport.rtcp_channel), make_rtcp_sender_report(
                message.ssrc, clock.ntp_at(now),
                MediaClock::rtp_at(track.anchor_timestamp, track.anchor_time, track.clock_rate, now),
                track.packets, track.octets, "rtsp-cpp", message.final_rtcp));
        }
    }
    throw std::logic_error("RTCP output has no matching track");
}

bool MediaSubscription::receive_rtcp(unsigned int channel, const std::string& wire) {
    const auto packets = parse_rtcp_compound(wire);  // 完整校验后再更新反馈和保活。
    bool matched = false;
    for (auto& track : tracks_) {
        if (channel != static_cast<unsigned int>(track.transport.rtcp_channel)) {
            continue;
        }
        for (const auto& packet : packets) {
            if (packet.type != 201 || packet.ssrc == 0) {
                continue;
            }
            for (const auto& report : packet.reports) {
                if (report.ssrc == track.sender->ssrc() && track.packets != 0) {
                    track.receiver = report;
                    matched = true;
                }
            }
        }
    }
    return matched;
}

std::optional<RtcpReceptionReport> MediaSubscription::receiver_report(std::uint32_t ssrc) const {
    for (const auto& track : tracks_) {
        if (track.sender->ssrc() == ssrc) {
            return track.receiver;
        }
    }
    return std::nullopt;
}

bool MediaSubscription::finished() const {
    return !active_;
}

bool MediaSubscription::failed() const {
    return failed_;
}

void MediaSubscription::deliver(std::size_t track_index, const RtpPayloadUnit& unit,
                                 MediaClock::Clock::time_point media_at) {
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
        if (media_pts_less(unit.pts, unit.clock_rate, start_pts_, start_clock_rate_)) {
            return;
        }
        const auto base = rescale_media_pts(start_pts_, start_clock_rate_, unit.clock_rate);
        const auto timestamp = track.timestamp_origin + static_cast<std::uint32_t>(unit.pts)
                               - static_cast<std::uint32_t>(base);
        try {
            if (track.sender->send_payloads(unit, timestamp, *queue_)) {
                if (!track.queued_rtp) {
                    track.anchor_timestamp = timestamp;
                    track.anchor_time = media_at;
                    track.queued_rtp = true;
                }
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
    if (!active_) {
        return;  // 已取消的订阅由连接自行关闭队列，不能抢在 TEARDOWN 响应前关闭。
    }
    failed_ = failed_ || failed;
    if (!started_ && has_video_ && !failed) {
        std::cout << "Subscription ended before an IDR became available" << std::endl;
    }
    active_ = false;
    if (!failed_) {
        end();
    }
    // 正常 EOF 允许事件循环排空队列；异常由连接处理路径中断。
    queue_->close();
}

MediaSession::MediaSession(std::unique_ptr<VideoSource> video_source,
                           std::unique_ptr<AudioSource> audio_source,
                           CaptureTimeline timeline,
                           std::chrono::milliseconds send_buffer)
    : video_source_(std::move(video_source)), audio_source_(std::move(audio_source)),
      timeline_(timeline), send_buffer_(send_buffer) {
    if (!video_source_ || !audio_source_ || send_buffer_.count() < 0) {
        throw std::invalid_argument("live session requires camera, microphone and a nonnegative send buffer");
    }
    video_source_->check_health();
    audio_source_->check_health();
    description_.tracks = {video_source_->track(), audio_source_->track()};
}

MediaSession::~MediaSession() {
    stop();
}

const SessionDescription& MediaSession::description() const {
    return description_;
}

SubscribeResult MediaSession::subscribe(const std::shared_ptr<MediaSubscription>& subscription,
                                        const std::string& play_response) {
    if (!subscription) {
        throw std::invalid_argument("expected a media subscription");
    }
    if (ended_) {
        return SubscribeResult::Ended;
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
        video_.reset();
        audio_.reset();
        video_source_->resume();
        waiting_for_idr_ = true;
        // 音频保持有界队列，避免删掉新 IDR 之后本来可发送的音频。
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

const MediaClock& MediaSession::clock() const {
    return clock_;
}

void MediaSession::finish(bool failed) {
    if (ended_) {
        return;
    }
    ended_ = true;
    video_source_->stop();
    audio_source_->stop();
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
        // 即使没有观众，也要在任一设备停止时回收两路资源。
        video_source_->check_health();
        audio_source_->check_health();
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
            next_deadline_.reset();
            return;
        }

        // 每轮限制处理量，避免积压的 AU 占住网络事件循环。
        for (int budget = 0; budget < 32; ++budget) {
            // 每条轨道预读一个 AU。同一份 RTP 负载只封装一次。
            if (!video_) {
                H264AccessUnit frame;
                if (video_source_->read(frame)) {
                    video_ = video_packetizer_.packetize_access_unit(frame);
                }
            }
            if (!audio_) {
                AacAccessUnit frame;
                if (audio_source_->read(frame)) {
                    audio_ = audio_packetizer_.packetize(frame, description_.tracks[audio_track].clock_rate);
                }
            }
            if (video_source_->ended() || audio_source_->ended()) {
                throw std::runtime_error(video_source_->ended() ? "video input ended" : "audio input ended");
            }
            if (!video_ && !audio_) {
                next_deadline_ = now + input_retry;  // 暂时没帧，不阻塞或忙等。
                return;
            }

            const bool send_video = video_ && (!audio_ || !media_pts_less(
                audio_->pts, audio_->clock_rate, video_->pts, video_->clock_rate));
            auto& pending = send_video ? video_ : audio_;
            const auto& unit = *pending;
            const auto media_time = std::chrono::microseconds(rescale_media_pts(unit.pts, unit.clock_rate, 1000000));
            const auto media_at = timeline_.at(media_time);
            const auto send_at = media_at + send_buffer_;
            if (now < send_at) {
                // 一轨已有未来帧时，也要及时重查另一轨。
                next_deadline_ = video_ && audio_ ? send_at : std::min(send_at, now + input_retry);
                return;
            }

            const bool late = now > send_at + max_live_lateness;
            if (send_video && late) {
                waiting_for_idr_ = true;
            }
            if (!late && (!send_video || !waiting_for_idr_ || unit.is_keyframe)) {
                if (send_video) {
                    waiting_for_idr_ = false;
                }
                for (const auto& subscriber : subscribers) {
                    subscriber->deliver(send_video ? video_track : audio_track, unit, media_at);
                }
                // deliver() 可能关闭慢客户端的队列，全部退出时不再消费媒体。
                if (std::all_of(subscribers.begin(), subscribers.end(), [](const auto& subscriber) {
                        return subscriber->finished();
                    })) {
                    pending.reset();
                    next_deadline_.reset();
                    return;
                }
            }
            pending.reset();
        }
        next_deadline_ = now;
    } catch (const std::exception& error) {
        std::cerr << "MediaSession error: " << error.what() << '\n';
        finish(true);
    }
}

}  // namespace rtsp
