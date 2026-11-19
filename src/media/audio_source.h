#pragma once

#include "media/aac_access_unit.h"
#include "sdp/session_description.h"

namespace rtsp {

// 由 MediaSession 所属事件循环线程读取；采集、编码和队列由具体源管理。
class AudioSource {
public:
    virtual ~AudioSource() = default;

    // 不等待新帧。true 表示取得完整 AAC AU；false 时用 ended() 区分等待和结束。
    // 不可恢复的读取错误通过异常报告。
    virtual bool read(AacAccessUnit& frame) = 0;
    virtual bool ended() const = 0;
    // 构造完成后即可生成 SDP；轨道配置不变，引用在源的生命周期内有效。
    virtual const MediaTrack& track() const = 0;
    // 不消费帧；必需实时源的生产者退出时抛异常。
    virtual void check_health() const = 0;
    // 由事件循环线程调用；可重复调用，收尾不得抛出异常。
    virtual void stop() = 0;
};

}  // namespace rtsp
