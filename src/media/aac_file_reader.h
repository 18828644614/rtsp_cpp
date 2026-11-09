#pragma once

#include "media/aac_access_unit.h"
#include "sdp/session_description.h"

#include <cstddef>
#include <string>
#include <vector>

namespace rtsp {

// 与 H264FileReader 一样，启动时读取并验证整个短样例文件。
class AacFileReader {
public:
    // path 使用 UTF-8，与服务器的 Windows 宽字符入口一致。
    void open(const std::string& path);
    bool read(AacAccessUnit& frame);
    const MediaTrack& track() const;
    bool ended() const;

private:
    MediaTrack track_;
    std::vector<AacAccessUnit> frames_;
    std::size_t next_frame_ = 0;
};

}  // namespace rtsp
