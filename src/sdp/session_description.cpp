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

// 生成与解析共用检查，避免默认值掩盖缺失字段。
void check_track(const MediaTrack& track) {
    check_line_value(track.control);
    check_line_value(track.fmtp);
    if (track.payload_type < 96 || track.payload_type > 127 || track.control.empty()
        || track.control == "*" || track.control.find_first_of(" \t") != std::string::npos) {
        throw std::runtime_error("invalid SDP track control or payload type");
    }
    if (track.media_type == "video" && track.encoding == "H264"
        && track.clock_rate == 90000 && track.channels == 0) {
        return;
    }
    if (track.media_type == "audio" && track.encoding == "MPEG4-GENERIC"
        && track.clock_rate > 0 && track.channels >= 1 && track.channels <= 8
        && !track.fmtp.empty()) {
        return;
    }
    throw std::runtime_error("expected H264/90000 or MPEG4-GENERIC audio");
}

void check_description(const SessionDescription& description) {
    check_line_value(description.control);
    if (description.tracks.empty()) {
        throw std::runtime_error("SDP requires at least one track");
    }
    for (std::size_t i = 0; i < description.tracks.size(); ++i) {
        check_track(description.tracks[i]);
        for (std::size_t j = 0; j < i; ++j) {
            if (description.tracks[i].control == description.tracks[j].control
                || description.tracks[i].payload_type == description.tracks[j].payload_type) {
                throw std::runtime_error("duplicate SDP control or payload type");
            }
        }
    }
}

std::string make_sdp(const SessionDescription& description) {
    check_description(description);
    std::string text = "v=0\r\n"
                       "o=- 0 0 IN IP4 127.0.0.1\r\n"
                       "s=RTSP learning session\r\n"
                       "c=IN IP4 0.0.0.0\r\n"
                       "t=0 0\r\n";
    if (!description.control.empty()) {
        text += "a=control:" + description.control + "\r\n";
    }
    for (const MediaTrack& track : description.tracks) {
        const std::string pt = std::to_string(track.payload_type);
        text += "m=" + track.media_type + " 0 RTP/AVP " + pt + "\r\n";
        text += "a=rtpmap:" + pt + " " + track.encoding + "/" + std::to_string(track.clock_rate);
        if (track.media_type == "audio") {
            text += "/" + std::to_string(track.channels);
        }
        text += "\r\n";
        if (!track.fmtp.empty()) {
            text += "a=fmtp:" + pt + " " + track.fmtp + "\r\n";
        }
        text += "a=control:" + track.control + "\r\n";
    }
    return text;
}

SessionDescription parse_sdp(const std::string& text) {
    SessionDescription description;
    description.control.clear();
    description.tracks.clear();
    bool has_version = false;
    bool has_session_control = false;
    bool has_rtpmap = false;
    bool has_fmtp = false;
    bool has_track_control = false;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line.size() < 2 || line[1] != '=' || line.find('\r') != std::string::npos) {
            throw std::runtime_error("invalid SDP line");
        }
        if (line[0] == 'v') {
            if (has_version || line != "v=0" || !description.tracks.empty()) {
                throw std::runtime_error("unsupported SDP version");
            }
            has_version = true;
        } else if (line[0] == 'm') {
            if (!description.tracks.empty() && (!has_rtpmap || !has_track_control)) {
                throw std::runtime_error("incomplete SDP track");
            }
            std::istringstream fields(line.substr(2));
            MediaTrack track;
            std::string port, protocol, pt, extra;
            if (!(fields >> track.media_type >> port >> protocol >> pt) || fields >> extra
                || protocol != "RTP/AVP") {
                throw std::runtime_error("expected one RTP/AVP payload type per track");
            }
            read_number(port, 65535);
            track.payload_type = read_number(pt, 127);
            track.control.clear();
            track.encoding.clear();
            track.clock_rate = 0;
            track.channels = 0;
            track.fmtp.clear();
            description.tracks.push_back(track);
            has_rtpmap = false;
            has_fmtp = false;
            has_track_control = false;
        } else if (line.compare(0, 10, "a=control:") == 0) {
            const std::string control = line.substr(10);
            if (control.empty() || control.find_first_of(" \t") != std::string::npos) {
                throw std::runtime_error("invalid SDP control value");
            }
            if (description.tracks.empty()) {
                if (has_session_control) {
                    throw std::runtime_error("duplicate session control");
                }
                description.control = control;
                has_session_control = true;
            } else {
                if (has_track_control || control == "*") {
                    throw std::runtime_error("invalid or duplicate media control");
                }
                description.tracks.back().control = control;
                has_track_control = true;
            }
        } else if (line.compare(0, 9, "a=rtpmap:") == 0
                   || line.compare(0, 7, "a=fmtp:") == 0) {
            const bool is_rtpmap = line.compare(0, 9, "a=rtpmap:") == 0;
            const std::size_t begin = is_rtpmap ? 9 : 7;
            const std::size_t space = line.find(' ', begin);
            if (description.tracks.empty() || space == std::string::npos
                || read_number(line.substr(begin, space - begin), 127)
                       != description.tracks.back().payload_type) {
                throw std::runtime_error("SDP attribute does not match the media payload");
            }
            MediaTrack& track = description.tracks.back();
            const std::string value = line.substr(space + 1);
            if (is_rtpmap) {
                const std::size_t slash = value.find('/');
                if (has_rtpmap || slash == std::string::npos) {
                    throw std::runtime_error("invalid or duplicate rtpmap");
                }
                track.encoding = value.substr(0, slash);
                const std::size_t second = value.find('/', slash + 1);
                track.clock_rate = read_number(value.substr(slash + 1,
                    second == std::string::npos ? std::string::npos : second - slash - 1), 192000);
                if (second != std::string::npos) {
                    track.channels = read_number(value.substr(second + 1), 8);
                } else if (track.media_type == "audio") {
                    track.channels = 1;
                }
                has_rtpmap = true;
            } else {
                if (has_fmtp || value.empty()) {
                    throw std::runtime_error("invalid or duplicate fmtp");
                }
                track.fmtp = value;
                has_fmtp = true;
            }
        }
    }
    if (!has_version || !has_rtpmap || !has_track_control) {
        throw std::runtime_error("SDP is missing track configuration");
    }
    check_description(description);
    return description;
}

}  // namespace rtsp
