#pragma once

#include <fstream>
#include <string>

namespace rtsp {

class H264FileWriter {
public:
    void open(const std::string& path);
    void write(const std::string& nalu);
    void close();

private:
    std::ofstream output_;
};

}
