#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace rtsp {

struct H264AccessUnit {
    std::vector<std::string> nalus;
};

class H264FileReader {
public:
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
