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
    // outbound_queue_,                             // ① 本客户端的发送队列
    // MediaTransport(next_session.transport()),    // ② 本客户端的媒体传输配置,例如：RTP 通道：0，RTCP 通道：1，再用它构造 MediaTransport，后续给 RTP 数据添加 TCP interleaved 头时，需要使用这里的通道号
    // description.video.payload_type,              // ③ RTP 负载类型,例如 96 h264
    // static_cast<std::uint16_t>(100),             // ④ RTP 初始序号
    // make_ssrc()                                  // ⑤ RTP 流标识 不同的订阅生成不同的 ssrc
    MediaSubscription(std::shared_ptr<net::OutboundQueue> queue, MediaTransport transport,
                      int payload_type, std::uint16_t sequence, std::uint32_t ssrc);
    ~MediaSubscription();

    // 返回后，即使调度线程已经取得旧快照，也不会再向此订阅投递。
    void cancel();
    bool finished() const;
    bool failed() const;

private:
    friend class MediaSession;
    // 先把 PLAY 成功响应放入队列
    //              ↓
    // 再设置 active_ = true
    //              ↓
    // 之后才能接受视频数据
    bool start(const std::string& play_response);
    // 检查状态，将媒体数据打包并入队
    void deliver(const std::shared_ptr<const H264PayloadUnit>& unit);
    void finish(bool failed);

    // 协调媒体线程投递、连接线程取消等操作
    mutable std::mutex mutex_;
    // 保存准备发给客户端的数据
    std::shared_ptr<net::OutboundQueue> queue_;
    // 维护序号、SSRC、时间戳起点等，组装数据并入队
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
// MediaSession 负责管理一个视频资源的播放过程：什么时候读下一帧、把这一帧分发给哪些订阅，以及什么时候结束
// MediaSession：管理整个视频的播放
// MediaSubscription：管理其中一个客户端的接收

//              一个 MediaSession
//              当前读到第 100 帧
//                     │
//          读一帧、生成一份媒体负载
//                     │
//          ┌──────────┴──────────┐
//          ▼                     ▼
// MediaSubscription A   MediaSubscription B
//          │                     │
//       客户端 A              客户端 B

// 没有有效订阅时，媒体线程等待，文件进度暂停
// 文件播放结束后，该会话不再接受新订阅
class MediaSession {
public:
    explicit MediaSession(const std::string& input_path);
    ~MediaSession();

    const SessionDescription& description() const;
    // 在同一临界区检查资源状态、将 PLAY 响应入队、注册订阅，防止与 EOF 竞争。
    // subscription: 准备加入的客户端订阅,
    // play_response: 准备放进发送队列的 PLAY 响应,
    // from_start: 是否要求从起点开始
    SubscribeResult subscribe(const std::shared_ptr<MediaSubscription>& subscription,
                              const std::string& play_response, bool from_start = false);
    bool ended() const;
    void stop();

private:
    // 项目的媒体调度线程函数：它按照播放时间读取 H.264 帧，把帧拆成 RTP 负载，再分发给当前订阅的客户端
    void run();
    void finish(bool failed);

    H264MediaSource source_;
    H264Packetizer packetizer_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::weak_ptr<MediaSubscription>> subscribers_;
    bool started_ = false;
    // 会话已经进入结束收尾，后续订阅被拒绝
    bool ended_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace rtsp
