#pragma once

#include "media/aac_file_reader.h"
#include "media/aac_packetizer.h"
#include "media/media_source.h"
#include "media/h264_packetizer.h"
#include "rtp/rtp_sender.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rtsp {

// 一个客户端的一次订阅：轨道独立维护 RTP 状态，共用客户端队列。
class MediaSubscription {
public:
    explicit MediaSubscription(std::shared_ptr<net::OutboundQueue> queue);
    ~MediaSubscription();

    // 仅在订阅前调用，为已经 SETUP 的轨道建立 Sender。
    void add_track(std::size_t index, const MediaTrack& track, TcpTransport transport,
                   std::uint16_t sequence, std::uint32_t ssrc);
    void cancel();
    bool finished() const;
    bool failed() const;

private:
    friend class MediaSession;
    struct TrackSender {
        std::size_t index = 0;
        std::unique_ptr<RtpSender> sender;
    };

    bool start(const std::string& play_response);
    void deliver(std::size_t track_index, const RtpPayloadUnit& unit);
    void finish(bool failed);

    mutable std::mutex mutex_;
    std::shared_ptr<net::OutboundQueue> queue_;
    std::vector<TrackSender> tracks_;
    bool has_video_ = false;
    bool active_ = false;
    bool started_ = false;
    bool failed_ = false;
    // 所有轨道使用同一个媒体起点，各自换算为本轨道的 RTP tick。
    std::uint64_t start_pts_ = 0;
    int start_clock_rate_ = 1;
};

enum class SubscribeResult {
    Started,
    Ended,
    InvalidRange,
    QueueClosed
};

// 一个资源、一个调度线程、共享音视频进度。空路径表示不启用对应轨道。
class MediaSession {
public:
    explicit MediaSession(const std::string& video_path, const std::string& audio_path = "");
    ~MediaSession();

    const SessionDescription& description() const;
    SubscribeResult subscribe(const std::shared_ptr<MediaSubscription>& subscription,
                              const std::string& play_response, bool from_start = false);
    bool ended() const;
    void stop();

private:
    void run();
    void finish(bool failed);

    SessionDescription description_;
    std::unique_ptr<H264MediaSource> video_source_;
    std::unique_ptr<AacFileReader> audio_source_;
    H264Packetizer video_packetizer_;
    AacPacketizer audio_packetizer_;
    std::size_t video_track_ = 0;
    std::size_t audio_track_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::weak_ptr<MediaSubscription>> subscribers_;
    bool started_ = false;
    bool ended_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace rtsp
