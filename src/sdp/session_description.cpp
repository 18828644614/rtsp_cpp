#include "sdp/session_description.h"

#include <sstream>
#include <stdexcept>

namespace rtsp {
namespace {

int read_number(const std::string& text, int maximum) {
    if (text.empty()) {
        throw std::runtime_error("empty SDP number");
    }
    int number = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const int digit = text[i] - '0';
        if (digit < 0 || digit > 9 || number > (maximum - digit) / 10) {
            throw std::runtime_error("invalid SDP number");
        }
        number = number * 10 + digit;
    }
    return number;
}

std::string base64_encode(const std::string& data) {
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t offset = 0; offset < data.size(); offset += 3) {
        const unsigned int first = static_cast<unsigned char>(data[offset]);
        unsigned int second = 0;
        unsigned int third = 0;
        if (offset + 1 < data.size()) {
            second = static_cast<unsigned char>(data[offset + 1]);
        }
        if (offset + 2 < data.size()) {
            third = static_cast<unsigned char>(data[offset + 2]);
        }
        result += alphabet[first >> 2];
        result += alphabet[((first & 3) << 4) | (second >> 4)];
        result += offset + 1 < data.size() ? alphabet[((second & 15) << 2) | (third >> 6)] : '=';
        result += offset + 2 < data.size() ? alphabet[third & 63] : '=';
    }
    return result;
}

void check_line_value(const std::string& value) {
    if (value.find_first_of("\r\n") != std::string::npos) {
        throw std::runtime_error("line break in SDP value");
    }
}

}  // namespace

std::string make_h264_fmtp(const std::string& sps, const std::string& pps) {
    if (sps.size() < 4 || pps.empty()
        || (static_cast<unsigned char>(sps[0]) & 0x9f) != 7
        || (static_cast<unsigned char>(pps[0]) & 0x9f) != 8) {
        throw std::runtime_error("invalid SPS/PPS for H264 SDP");
    }
    const std::string digits = "0123456789abcdef";
    std::string profile;
    for (std::size_t index = 1; index <= 3; ++index) {
        const unsigned int value = static_cast<unsigned char>(sps[index]);
        profile += digits[value >> 4];
        profile += digits[value & 15];
    }
    return "packetization-mode=1;profile-level-id=" + profile
           + ";sprop-parameter-sets=" + base64_encode(sps) + "," + base64_encode(pps);
}

std::string make_sdp(const SessionDescription& description) {
    const MediaTrack& video = description.video;
    if (video.encoding != "H264" || video.clock_rate != 90000
        || video.payload_type < 96 || video.payload_type > 127
        || video.control.empty() || video.control == "*") {
        throw std::runtime_error("only one dynamic H264/90000 track is supported");
    }
    check_line_value(description.control);
    check_line_value(video.control);
    check_line_value(video.fmtp);
    const std::string pt = std::to_string(video.payload_type);
    std::string text = "v=0\r\n"
                       "o=- 0 0 IN IP4 127.0.0.1\r\n"
                       "s=RTSP learning session\r\n"
                       "c=IN IP4 0.0.0.0\r\n"
                       "t=0 0\r\n";
    text += "a=control:" + description.control + "\r\n";
    text += "m=video 0 RTP/AVP " + pt + "\r\n";
    text += "a=rtpmap:" + pt + " H264/90000\r\n";
    if (!video.fmtp.empty()) {
        text += "a=fmtp:" + pt + " " + video.fmtp + "\r\n";
    }
    text += "a=control:" + video.control + "\r\n";
    return text;
}

SessionDescription parse_sdp(const std::string& text) {
    SessionDescription description;
    description.control = "";
    description.video.control = "";
    description.video.encoding = "";
    description.video.clock_rate = 0;
    description.video.fmtp = "";
    bool has_version = false;
    bool has_media = false;
    bool has_rtpmap = false;
    bool has_fmtp = false;
    bool has_session_control = false;
    bool has_track_control = false;

    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        if (line.empty()) {
            continue;
        }
        if (line.size() < 2 || line[1] != '=' || line.find('\r') != std::string::npos) {
            throw std::runtime_error("invalid SDP line");
        }
        if (line[0] == 'v') {
            if (has_version || line != "v=0" || has_media) {
                throw std::runtime_error("unsupported SDP version");
            }
            has_version = true;
        } else if (line[0] == 'm') {
            if (has_media) {
                throw std::runtime_error("only one media track is supported");
            }
            std::istringstream fields(line.substr(2));
            std::string type, port, protocol, pt, extra;
            if (!(fields >> type >> port >> protocol >> pt) || fields >> extra
                || type != "video" || protocol != "RTP/AVP") {
                throw std::runtime_error("expected one video RTP/AVP payload type");
            }
            read_number(port, 65535);
            description.video.payload_type = read_number(pt, 127);
            if (description.video.payload_type < 96) {
                throw std::runtime_error("H264 requires a dynamic payload type");
            }
            has_media = true;
        } else if (line.compare(0, 10, "a=control:") == 0) {
            const std::string control = line.substr(10);
            if (control.empty() || control.find_first_of(" \t") != std::string::npos) {
                throw std::runtime_error("invalid SDP control value");
            }
            if (has_media) {
                if (has_track_control || control == "*") {
                    throw std::runtime_error("invalid or duplicate media control");
                }
                description.video.control = control;
                has_track_control = true;
            } else {
                if (has_session_control) {
                    throw std::runtime_error("duplicate session control");
                }
                description.control = control;
                has_session_control = true;
            }
        } else if (line.compare(0, 9, "a=rtpmap:") == 0
                   || line.compare(0, 7, "a=fmtp:") == 0) {
            const bool is_rtpmap = line.compare(0, 9, "a=rtpmap:") == 0;
            const std::size_t begin = is_rtpmap ? 9 : 7;
            const std::size_t space = line.find(' ', begin);
            if (!has_media || space == std::string::npos
                || read_number(line.substr(begin, space - begin), 127)
                       != description.video.payload_type) {
                throw std::runtime_error("SDP attribute does not match the media payload");
            }
            const std::string value = line.substr(space + 1);
            if (is_rtpmap) {
                if (has_rtpmap || value != "H264/90000") {
                    throw std::runtime_error("expected one H264/90000 mapping");
                }
                description.video.encoding = "H264";
                description.video.clock_rate = 90000;
                has_rtpmap = true;
            } else {
                if (has_fmtp || value.empty()) {
                    throw std::runtime_error("invalid or duplicate fmtp");
                }
                description.video.fmtp = value;
                has_fmtp = true;
            }
        }
        // o/s/c/t 等行和未知属性暂不参与轨道协商，保留扩展空间。
    }
    if (!has_version || !has_media || !has_rtpmap || !has_track_control) {
        throw std::runtime_error("SDP is missing video configuration");
    }
    return description;
}

}  // namespace rtsp
