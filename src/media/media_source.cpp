#include "media/media_source.h"

#include <stdexcept>
#include <utility>

namespace rtsp {

H264MediaSource::H264MediaSource(const std::string& path) {
    reader_.open(path);
    description_.tracks[0].fmtp = make_h264_fmtp(reader_.sps(), reader_.pps());
}

const SessionDescription& H264MediaSource::description() const {
    return description_;
}

bool H264MediaSource::read(H264AccessUnit& frame) {
    if (!reader_.read(frame)) {
        return false;
    }
    ++read_count_;

    bool has_sps = false;
    bool has_pps = false;
    for (const std::string& nalu : frame.nalus) {
        const unsigned int type = static_cast<unsigned char>(nalu[0]) & 0x1f;
        if (type == 7) {
            sps_ = nalu;
            has_sps = true;
        } else if (type == 8) {
            pps_ = nalu;
            has_pps = true;
        } else if (type >= 1 && type <= 5 && (sps_.empty() || pps_.empty())) {
            throw std::runtime_error("H264 input requires SPS/PPS before pictures");
        }
    }

    // 给缺少参数集的 IDR 补齐参数。所有客户端复用同一份结果，只封装一次。
    if (frame.is_keyframe && (!has_sps || !has_pps)) {
        std::vector<std::string> nalus;
        std::size_t begin = 0;
        while (begin < frame.nalus.size()
               && (static_cast<unsigned char>(frame.nalus[begin][0]) & 0x1f) == 9) {
            nalus.push_back(std::move(frame.nalus[begin++]));
        }
        nalus.push_back(sps_);
        nalus.push_back(pps_);
        for (; begin < frame.nalus.size(); ++begin) {
            const unsigned int type = static_cast<unsigned char>(frame.nalus[begin][0]) & 0x1f;
            if (type != 7 && type != 8) {
                nalus.push_back(std::move(frame.nalus[begin]));
            }
        }
        frame.nalus = std::move(nalus);
    }
    return true;
}

bool H264MediaSource::ended() const {
    return read_count_ == reader_.frame_count();
}

}  // namespace rtsp
