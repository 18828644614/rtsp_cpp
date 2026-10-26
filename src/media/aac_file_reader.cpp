#include "media/aac_file_reader.h"

#include <fstream>
#include <stdexcept>

namespace rtsp {
namespace {

const int sample_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000, 7350
};

std::string make_aac_fmtp(int frequency_index, int channels) {
    // AudioSpecificConfig：5 位 object type + 4 位采样率索引 + 4 位声道配置 + 3 位零。
    const unsigned int config = (2 << 11) | (frequency_index << 7) | (channels << 3);
    const std::string hex = "0123456789abcdef";
    std::string value;
    for (int shift = 12; shift >= 0; shift -= 4) {
        value += hex[(config >> shift) & 15];
    }
    return "streamtype=5;profile-level-id=1;mode=AAC-hbr;config=" + value
        + ";SizeLength=13;IndexLength=3;IndexDeltaLength=3";
}

}  // namespace

void AacFileReader::open(const std::string& path) {
    frames_.clear();
    next_frame_ = 0;
    track_ = MediaTrack();
    track_.media_type = "audio";
    track_.encoding = "MPEG4-GENERIC";
    track_.control = "trackID=1";
    track_.payload_type = 97;

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open AAC input: " + path);
    }
    const std::streamoff size = input.tellg();
    if (size <= 0 || size > 64 * 1024 * 1024) {
        throw std::runtime_error("AAC sample must contain 1 byte to 64 MiB");
    }
    std::string data(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    input.read(&data[0], size);
    if (!input) {
        throw std::runtime_error("failed to read AAC input");
    }

    int first_frequency = -1;
    int first_channels = 0;
    std::size_t offset = 0;
    std::vector<AacAccessUnit> frames;
    while (offset < data.size()) {
        if (data.size() - offset < 7) {
            throw std::runtime_error("truncated ADTS header");
        }
        unsigned char header[7];
        for (int i = 0; i < 7; ++i) {
            header[i] = static_cast<unsigned char>(data[offset + i]);
        }
        // ID 可为 MPEG-2 或 MPEG-4；layer 必须为 0。
        if (header[0] != 0xff || (header[1] & 0xf6) != 0xf0) {
            throw std::runtime_error("expected ADTS sync word and layer 0");
        }
        const int profile = (header[2] >> 6) + 1;
        const int frequency = (header[2] >> 2) & 15;
        const int channels = ((header[2] & 1) << 2) | (header[3] >> 6);
        const std::size_t header_size = (header[1] & 1) != 0 ? 7 : 9;
        const std::size_t frame_size = ((header[3] & 3) << 11)
            | (header[4] << 3) | (header[5] >> 5);
        if (profile != 2 || frequency >= 13 || channels == 0 || (header[6] & 3) != 0) {
            throw std::runtime_error("only AAC-LC, explicit channels and one raw block are supported");
        }
        if (frame_size <= header_size || frame_size > data.size() - offset) {
            throw std::runtime_error("invalid or truncated ADTS frame");
        }
        // 一个 AU 放入一个 TCP RTP 包；同时符合 13 位 AU-size 和 16 位交织帧长度。
        if (frame_size - header_size > 8191) {
            throw std::runtime_error("AAC AU is too large");
        }
        if (first_frequency < 0) {
            first_frequency = frequency;
            first_channels = channels;
        } else if (frequency != first_frequency || channels != first_channels) {
            throw std::runtime_error("AAC configuration changed inside the file");
        }
        AacAccessUnit frame;
        frame.data = data.substr(offset + header_size, frame_size - header_size);
        frame.pts = static_cast<std::uint64_t>(frames.size()) * 1024;
        frames.push_back(frame);
        offset += frame_size;
    }
    track_.clock_rate = sample_rates[first_frequency];
    // ADTS 声道配置 7 表示 7.1，共 8 声道。
    track_.channels = first_channels == 7 ? 8 : first_channels;
    track_.fmtp = make_aac_fmtp(first_frequency, first_channels);
    frames_.swap(frames);
}

bool AacFileReader::read(AacAccessUnit& frame) {
    frame = AacAccessUnit();
    if (ended()) {
        return false;
    }
    frame = frames_[next_frame_++];
    return true;
}

const MediaTrack& AacFileReader::track() const {
    return track_;
}

bool AacFileReader::ended() const {
    return next_frame_ == frames_.size();
}

}  // namespace rtsp
