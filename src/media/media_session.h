#pragma once

#include "media/media_source.h"
#include "rtp/rtp_sender.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rtsp {

// 一个客户端的一次订阅。不保存连接指针，队列可以安全活到最后一次投递结束。
class MediaSubscription {
public:
    MediaSubscription(std::shared_ptr<net::OutboundQueue> queue, MediaTransport transport,
                      int payload_type, std::uint16_t sequence, std::uint32_t ssrc);
    ~MediaSubscription();

    // 返回后，即使调度线程已经取得旧快照，也不会再向此订阅投递。
    void cancel();
    bool finished() const;
    bool failed() const;

private:
    friend class MediaSession;
    bool start(const std::string& play_response);
    void deliver(const std::shared_ptr<const H264PayloadUnit>& unit);
    void finish(bool failed);

    mutable std::mutex mutex_;
    std::shared_ptr<net::OutboundQueue> queue_;
    RtpSender sender_;
    bool active_ = false;
    bool waiting_for_idr_ = true;
    bool failed_ = false;
};

enum class SubscribeResult {
    Started,
    Ended,
    InvalidRange,
    QueueClosed
};

// 一个资源、一个文件游标、一个调度线程。订阅者共享进度，RTP 状态各自独立。
class MediaSession {
public:
    explicit MediaSession(const std::string& input_path);
    ~MediaSession();

    const SessionDescription& description() const;
    // 在同一临界区检查资源状态、将 PLAY 响应入队、注册订阅，防止与 EOF 竞争。
    SubscribeResult subscribe(const std::shared_ptr<MediaSubscription>& subscription,
                              const std::string& play_response, bool from_start = false);
    bool ended() const;
    void stop();

private:
    void run();
    void finish(bool failed);

    H264MediaSource source_;
    H264Packetizer packetizer_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::weak_ptr<MediaSubscription>> subscribers_;
    bool started_ = false;
    bool ended_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace rtsp
