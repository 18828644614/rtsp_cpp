#pragma once

#include "media/h264_file_reader.h"
#include "sdp/session_description.h"

#include <string>

namespace rtsp {

// 文件只在构造时打开一次；之后由事件循环调用 MediaSession::tick 推进游标。
class H264MediaSource {
public:
    explicit H264MediaSource(const std::string& path);

    const SessionDescription& description() const;
    bool read(H264AccessUnit& frame);
    bool ended() const;

private:
    H264FileReader reader_;
    SessionDescription description_;
    std::string sps_;
    std::string pps_;
    std::size_t read_count_ = 0;
};

}  // namespace rtsp
