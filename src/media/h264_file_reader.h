#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rtsp {

struct H264AccessUnit {
    std::vector<std::string> nalus;
};

// 00 00 00 01 09 f0
// 00 00 01    67 64 00 1f
// 00 00 00 01 68 ee
//分割后：(属于同一个H264AccessUnit)
// NALU 1: 09 f0
// NALU 2: 67 64 00 1f
// NALU 3: 68 ee
class H264FileReader {
public:
    //根据h264文件中的NALU，将他们根据类型为 AUD(9) 进行分割成访问单元 (H264AccessUnit)
    void open(const std::string& path);
    bool read(H264AccessUnit& frame);
    std::string sps() const;
    std::string pps() const;
    std::size_t frame_count() const;

private:
    std::vector<H264AccessUnit> frames_;
    std::size_t next_frame_ = 0;
    std::string sps_;
    std::string pps_;
};

}
