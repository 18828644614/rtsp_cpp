#pragma once

#include "media/h264_file_reader.h"
#include "media/video_source.h"

#include <string>

namespace rtsp {

// 文件只在构造时打开一次；之后由事件循环调用 MediaSession::tick 推进游标。
class FileVideoSource : public VideoSource {
public:
    explicit FileVideoSource(const std::string& path);

    bool read(H264AccessUnit& frame) override;
    bool ended() const override;
    const MediaTrack& track() const override;

private:
    H264FileReader reader_;
    MediaTrack track_;
    std::string sps_;
    std::string pps_;
    std::size_t read_count_ = 0;
};

}  // namespace rtsp
