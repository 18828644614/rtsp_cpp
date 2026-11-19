#pragma once

#include "media/h264_access_unit.h"
#include "sdp/session_description.h"

namespace rtsp {

// 由 MediaSession 所属事件循环线程读取；采集、编码和队列由具体源管理。
class VideoSource {
public:
    virtual ~VideoSource() = default;

    // 不阻塞等待新帧。true 表示取得一份完整 AU；false 时用 ended() 判断是否结束。
    // 不可恢复的读取错误通过异常报告。
    virtual bool read(H264AccessUnit& frame) = 0;
    // true 表示源已耗尽，不再产生新帧；false 也可能只是暂时无帧。
    virtual bool ended() const = 0;
    // 构造完成后即可用于生成 SDP；引用在源的生命周期内有效。
    virtual const MediaTrack& track() const = 0;
    // 不消费帧；必需实时源的生产者退出时抛异常。
    virtual void check_health() const = 0;
    // 无订阅者后的首次订阅；实时源丢弃积压并从可解码的 IDR 恢复。
    virtual void resume() = 0;
    // MediaSession 停止或结束时回收生产资源；由所属事件循环线程调用。
    virtual void stop() = 0;
};

}  // namespace rtsp
