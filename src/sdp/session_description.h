#pragma once

#include <string>

namespace rtsp {

// 第一版只描述一条 H.264 视频轨道，不提前引入多轨道选择逻辑。
struct MediaTrack {
    std::string control = "trackID=0";
    int payload_type = 96;
    std::string encoding = "H264";
    int clock_rate = 90000;
    std::string fmtp = "packetization-mode=1";
};

struct SessionDescription {
    std::string control = "*";  // 聚合控制：PLAY 和 TEARDOWN 使用整个会话的 URL。
    MediaTrack video;
};

std::string make_h264_fmtp(const std::string& sps, const std::string& pps);

//生成sdp消息
std::string make_sdp(const SessionDescription& description);

// 只支持单路 video / RTP/AVP / H264，格式非法或不支持时抛出异常。
// 解析来自服务器的 SDP，不能依赖 MediaTrack 中的默认值猜测缺失参数。
SessionDescription parse_sdp(const std::string& text);

}  // namespace rtsp
