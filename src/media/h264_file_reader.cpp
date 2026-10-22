#include "media/h264_file_reader.h"

#include <fstream>
#include <stdexcept>

namespace rtsp {
namespace {

std::size_t find_start_code(const std::string& data, std::size_t begin,
                            std::size_t& prefix_size) {
    for (std::size_t offset = begin; offset + 2 < data.size(); ++offset) {
        if (data[offset] != '\0' || data[offset + 1] != '\0') {
            continue;
        }
        if (data[offset + 2] == '\1') {
            prefix_size = 3;
            return offset;
        }
        if (offset + 3 < data.size() && data[offset + 2] == '\0'
            && data[offset + 3] == '\1') {
            prefix_size = 4;
            return offset;
        }
    }
    prefix_size = 0;
    return data.size();
}

}

void H264FileReader::open(const std::string& path) {
    frames_.clear();
    next_frame_ = 0;
    sps_.clear();
    pps_.clear();

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open H264 input: " + path);
    }
    const std::streamoff size = input.tellg();
    if (size <= 0 || size > 64 * 1024 * 1024) {
        throw std::runtime_error("H264 sample must contain 1 byte to 64 MiB");
    }
    std::string data(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    input.read(&data[0], size);
    if (!input) {
        throw std::runtime_error("failed to read H264 input: " + path);
    }

    std::vector<H264AccessUnit> frames;
    H264AccessUnit frame;
    bool has_aud = false;
    bool has_picture = false;
    std::size_t prefix_size = 0;
    std::size_t position = find_start_code(data, 0, prefix_size);
    if (position == data.size() || data.find_first_not_of('\0') < position) {
        throw std::runtime_error("expected an Annex-B H264 file");
    }
    while (position < data.size()) {
        const std::size_t begin = position + prefix_size;
        std::size_t next_prefix_size = 0;
        const std::size_t next = find_start_code(data, begin, next_prefix_size);
        std::size_t end = next;
        while (end > begin && data[end - 1] == '\0') {
            --end;
        }
        if (end == begin || end - begin > 4 * 1024 * 1024) {
            throw std::runtime_error("empty H264 NALU or NALU larger than 4 MiB");
        }
        const std::string nalu = data.substr(begin, end - begin);
        const unsigned int header = static_cast<unsigned char>(nalu[0]);
        const unsigned int type = header & 0x1f;
        if ((header & 0x80) != 0 || type == 0 || type > 23) {
            throw std::runtime_error("unsupported NALU in H264 file");
        }
        if (type == 9) {
            if (has_aud) {
                if (!has_picture) {
                    throw std::runtime_error("AUD access unit has no picture");
                }
                frames.push_back(frame);
                frame.nalus.clear();
                has_picture = false;
            }
            has_aud = true;
        }
        if (type >= 1 && type <= 5) {
            if (!has_aud) {
                throw std::runtime_error("H264 input requires AUD before every access unit");
            }
            has_picture = true;
        }
        if (type == 7 && sps_.empty()) {
            sps_ = nalu;
        }
        if (type == 8 && pps_.empty()) {
            pps_ = nalu;
        }
        frame.nalus.push_back(nalu);
        position = next;
        prefix_size = next_prefix_size;
    }
    if (!has_aud || !has_picture || sps_.size() < 4 || pps_.empty()) {
        throw std::runtime_error("H264 input requires AUD, pictures, SPS and PPS");
    }
    frames.push_back(frame);
    frames_ = frames;
}

bool H264FileReader::read(H264AccessUnit& frame) {
    frame.nalus.clear();
    if (next_frame_ == frames_.size()) {
        return false;
    }
    frame = frames_[next_frame_];
    ++next_frame_;
    return true;
}

std::string H264FileReader::sps() const {
    return sps_;
}

std::string H264FileReader::pps() const {
    return pps_;
}

std::size_t H264FileReader::frame_count() const {
    return frames_.size();
}

}
