#include "rtsp/rtsp_session_utils.h"

#include <sstream>
#include <stdexcept>

namespace rtsp {
namespace {

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

std::string lower(const std::string& text) {
    std::string result = text;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (result[i] >= 'A' && result[i] <= 'Z') {
            result[i] = static_cast<char>(result[i] - 'A' + 'a');
        }
    }
    return result;
}

bool read_channel(const std::string& text, int& channel) {
    if (text.empty()) {
        return false;
    }
    int number = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        number = number * 10 + text[i] - '0';
        if (number > 255) {
            return false;
        }
    }
    channel = number;
    return true;
}

// 本步支持常用的 rtsp://host[:port]/path，不实现认证和 IPv6 URL。
std::string url_origin(const std::string& url) {
    if (url.compare(0, 7, "rtsp://") != 0
        || url.find_first_of(" \t\r\n#") != std::string::npos) {
        throw std::runtime_error("expected an absolute RTSP URL");
    }
    const std::size_t end = url.find_first_of("/?", 7);
    const std::string authority = url.substr(7, end == std::string::npos
                                                 ? std::string::npos : end - 7);
    if (authority.empty() || authority.find_first_of("@[]\\") != std::string::npos) {
        throw std::runtime_error("unsupported RTSP URL authority");
    }
    return "rtsp://" + authority;
}

void check_control_path(const std::string& control) {
    // 本步没有实现路径归一化，遇到独立的 '.' / '..' 段时明确拒绝。
    std::istringstream path(control.substr(0, control.find('?')));
    std::string segment;
    while (std::getline(path, segment, '/')) {
        if (segment == "." || segment == "..") {
            throw std::runtime_error("dot segments in control URLs are not supported yet");
        }
    }
}

}  // namespace

bool parse_tcp_transport(const std::string& text, TcpTransport& transport) {
    // 多个备选 Transport 和 multicast 留到后续互通阶段。
    if (text.find(',') != std::string::npos) {
        return false;
    }
    std::istringstream input(text);
    std::string part;
    if (!std::getline(input, part, ';') || lower(trim(part)) != "rtp/avp/tcp") {
        return false;
    }

    TcpTransport parsed;
    bool has_unicast = false;
    bool has_channels = false;
    bool has_mode = false;
    while (std::getline(input, part, ';')) {
        part = lower(trim(part));
        if (part == "unicast" && !has_unicast) {
            has_unicast = true;
        } else if ((part == "mode=play" || part == "mode=\"play\"") && !has_mode) {
            has_mode = true;
        } else if (part.compare(0, 12, "interleaved=") == 0 && !has_channels) {
            const std::string pair = part.substr(12);
            const std::size_t dash = pair.find('-');
            if (dash == std::string::npos
                || !read_channel(pair.substr(0, dash), parsed.rtp_channel)
                || !read_channel(pair.substr(dash + 1), parsed.rtcp_channel)
                || parsed.rtp_channel == parsed.rtcp_channel) {
                return false;
            }
            has_channels = true;
        } else {
            return false;
        }
    }
    if (!has_unicast || !has_channels) {
        return false;
    }
    transport = parsed;
    return true;
}

std::string make_tcp_transport(const TcpTransport& transport) {
    if (transport.rtp_channel < 0 || transport.rtp_channel > 255
        || transport.rtcp_channel < 0 || transport.rtcp_channel > 255
        || transport.rtp_channel == transport.rtcp_channel) {
        throw std::runtime_error("invalid interleaved channels");
    }
    return "RTP/AVP/TCP;unicast;interleaved=" + std::to_string(transport.rtp_channel)
           + "-" + std::to_string(transport.rtcp_channel);
}

std::string session_id_from_header(const std::string& text) {
    return trim(text.substr(0, text.find(';')));
}

std::string rtsp_url_path(const std::string& url) {
    const std::string origin = url_origin(url);
    if (url.size() == origin.size()) {
        return "/";
    }
    return url.substr(origin.size());
}

std::string resolve_control_url(const std::string& base, const std::string& control) {
    const std::string origin = url_origin(base);
    if (control.empty() || control == "*") {
        return base;
    }
    check_control_path(control);
    if (control.compare(0, 7, "rtsp://") == 0) {
        url_origin(control);  // 先验证绝对地址。
        return control;
    }
    if (control.find_first_of(" \t\r\n#\\") != std::string::npos
        || control.find("://") != std::string::npos
        || control.compare(0, 2, "//") == 0) {
        throw std::runtime_error("unsupported SDP control URL");
    }
    if (control[0] == '/') {
        return origin + control;
    }

    // 去掉基地址的查询串，再将普通相对路径连接到基地址所在目录。
    const std::string clean_base = base.substr(0, base.find('?'));
    const std::size_t slash = clean_base.find_last_of('/');
    std::string directory = origin + "/";
    if (slash >= origin.size()) {
        directory = clean_base.substr(0, slash + 1);
    }
    return directory + control;
}

}  // namespace rtsp
