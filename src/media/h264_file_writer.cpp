#include "media/h264_file_writer.h"

#include <stdexcept>

namespace rtsp {

void H264FileWriter::open(const std::string& path) {
    if (output_.is_open()) {
        throw std::logic_error("H264 output is already open");
    }
    output_.open(path, std::ios::binary | std::ios::trunc);
    if (!output_) {
        throw std::runtime_error("cannot open H264 output: " + path);
    }
}

void H264FileWriter::write(const std::string& nalu) {
    if (!output_.is_open() || nalu.empty()) {
        throw std::runtime_error("H264 write requires an open file and a complete NALU");
    }
    output_.write("\0\0\0\1", 4);
    output_.write(nalu.data(), static_cast<std::streamsize>(nalu.size()));
    if (!output_) {
        throw std::runtime_error("failed to write H264 output");
    }
}

void H264FileWriter::close() {
    if (output_.is_open()) {
        output_.close();
        if (!output_) {
            throw std::runtime_error("failed to close H264 output");
        }
    }
}

}
