#pragma once

#include "media/audio_source.h"
#include "media/aac_packetizer.h"
#include "media/capture_timeline.h"
#include "media/video_source.h"
#include "media/h264_packetizer.h"
#include "rtp/rtp_sender.h"
#include "rtp/rtcp_packet.h"
#include "media/media_clock.h"
#include "rtsp/rtsp_session_utils.h"

#include <chrono>
#include <memory>
#include <string>
#include <optional>
#include <vector>

namespace rtsp {

// 一个客户端的一次订阅：轨道独立维护 RTP 状态，共用客户端队列。
// 所有访问均由所属事件循环线程串行执行，不提供线程同步。
class MediaSubscription {
public:
    explicit MediaSubscription(std::shared_ptr<net::OutboundQueue> queue);
    ~MediaSubscription();

    MediaSubscription(const MediaSubscription&) = delete;
    MediaSubscription& operator=(const MediaSubscription&) = delete;

    // 仅在订阅前调用，为已经 SETUP 的轨道建立 Sender。
    void add_track(std::size_t index, const MediaTrack& track, TcpTransport transport,
                   std::uint16_t sequence, std::uint32_t ssrc, std::uint32_t timestamp_origin = 0);
    void cancel();
    // 正常结束：停止生产并排入最终 SR/SDES/BYE，队列关闭仍由调用者决定。
    void end();
    bool finished() const;
    bool failed() const;
    std::string rtp_info(const std::string& base_url) const;
    void on_sent(const net::OutboundMessage& message);
    void tick_rtcp(MediaClock::Clock::time_point now, std::chrono::milliseconds interval);
    std::optional<MediaClock::Clock::time_point> next_rtcp_deadline() const;
    std::string prepare_rtcp(const net::OutboundMessage& message, const MediaClock& clock,
                              MediaClock::Clock::time_point now) const;
    bool receive_rtcp(unsigned int channel, const std::string& wire);
    std::optional<RtcpReceptionReport> receiver_report(std::uint32_t ssrc) const;

private:
    friend class MediaSession;
    struct TrackSender {
        std::size_t index = 0;
        std::unique_ptr<RtpSender> sender;
        TcpTransport transport;
        std::string control;
        int clock_rate = 90000;
        std::uint32_t timestamp_origin = 0;
        bool queued_rtp = false;
        bool final_queued = false;
        bool report_pending = false;
        std::uint32_t anchor_timestamp = 0;
        MediaClock::Clock::time_point anchor_time{};
        std::uint32_t packets = 0;
        std::uint32_t octets = 0;
        std::optional<MediaClock::Clock::time_point> next_report;
        std::optional<RtcpReceptionReport> receiver;
    };

    bool start(const std::string& play_response);
    void deliver(std::size_t track_index, const RtpPayloadUnit& unit,
                 MediaClock::Clock::time_point media_at);
    void finish(bool failed);
    bool queue_report(TrackSender& track, bool final);
    std::shared_ptr<net::OutboundQueue> queue_;
    std::vector<TrackSender> tracks_;
    bool has_video_ = false;
    bool active_ = false;
    bool started_ = false;
    bool failed_ = false;
    // 所有轨道使用同一个媒体起点，各自换算为本轨道的 RTP tick。
    std::int64_t start_pts_ = 0;
    int start_clock_rate_ = 1;
};

enum class SubscribeResult {
    Started,
    Ended,
    QueueClosed
};

// /live 的两路实时媒体；只能由所属事件循环线程推进。
class MediaSession {
public:
    using Clock = std::chrono::steady_clock;
    // 两个源必须已经准备好，且与调度器使用同一个 T0。
    MediaSession(std::unique_ptr<VideoSource> video_source,
                 std::unique_ptr<AudioSource> audio_source,
                 CaptureTimeline timeline,
                 std::chrono::milliseconds send_buffer = std::chrono::milliseconds{150});
    ~MediaSession();

    const SessionDescription& description() const;
    SubscribeResult subscribe(const std::shared_ptr<MediaSubscription>& subscription,
                              const std::string& play_response);
    bool ended() const;
    void stop();
    
    // 判断现在是否有媒体需要发送。
    // 读取并封装下一份音频或视频。
    // 把 RTP 数据放入订阅者的 OutboundQueue。
    void tick(Clock::time_point now);
    std::optional<Clock::time_point> next_deadline() const;
    const MediaClock& clock() const;

private:
    void finish(bool failed);

    SessionDescription description_;
    MediaClock clock_;
    std::unique_ptr<VideoSource> video_source_;
    std::unique_ptr<AudioSource> audio_source_;
    const CaptureTimeline timeline_;
    const std::chrono::milliseconds send_buffer_;
    H264Packetizer video_packetizer_;
    AacPacketizer audio_packetizer_;

    std::vector<std::weak_ptr<MediaSubscription>> subscribers_;
    bool ended_ = false;
    bool waiting_for_idr_ = false;
    std::optional<Clock::time_point> next_deadline_;
    // 各轨道保留一份待发负载，tick 内以常量引用同步分发，不转移所有权。
    std::optional<RtpPayloadUnit> video_;
    std::optional<RtpPayloadUnit> audio_;
};

}  // namespace rtsp
