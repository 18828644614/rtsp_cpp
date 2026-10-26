#pragma once

#include <string>
#include <vector>

namespace rtsp {

// 每个 m= 段对应一条轨道。第一版支持 H.264 和 AAC-LC。
struct MediaTrack {
    std::string media_type = "video";
    std::string control = "trackID=0";
    int payload_type = 96;
    std::string encoding = "H264";
    int clock_rate = 90000;
    int channels = 0;
    std::string fmtp = "packetization-mode=1";
};

struct SessionDescription {
    std::string control = "*";  // 聚合控制：PLAY 和 TEARDOWN 使用整个会话的 URL。
    std::vector<MediaTrack> tracks = {MediaTrack()};
};

std::string make_h264_fmtp(const std::string& sps, const std::string& pps);

//生成sdp消息
std::string make_sdp(const SessionDescription& description);

// 支持 H264/90000 和 MPEG4-GENERIC 音频，格式非法或不支持时抛出异常。
// 解析来自服务器的 SDP，不能依赖 MediaTrack 中的默认值猜测缺失参数。
SessionDescription parse_sdp(const std::string& text);

}  // namespace rtsp
