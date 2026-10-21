#include "rtsp/rtsp_message.h"

#include <stdexcept>

namespace rtsp {
namespace {

bool same_header_name(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::string trim_spaces(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

// 逐字符读取十进制数，先检查再相乘，避免整数溢出。
std::size_t parse_number(const std::string& text, std::size_t maximum) {
    if (text.empty()) {
        throw std::runtime_error("empty RTSP number");
    }
    std::size_t number = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            throw std::runtime_error("RTSP number must contain digits only");
        }
        const std::size_t digit = static_cast<std::size_t>(text[i] - '0');
        if (number > maximum / 10 || (number == maximum / 10 && digit > maximum % 10)) {
            throw std::runtime_error("RTSP number exceeds the allowed limit");
        }
        number = number * 10 + digit;
    }
    return number;
}

// 方法名和头字段名使用 HTTP/RTSP 的 token 字符，不接受空格或控制字符。
bool is_token(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    const std::string separators = "()<>@,;:\\\"/[]?={} \t";
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c <= 32 || c >= 127 || separators.find(text[i]) != std::string::npos) {
            return false;
        }
    }
    return true;
}

void check_text(const std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c < 32 && c != '\t') || c == 127) {
            throw std::runtime_error("control character in RTSP text");
        }
    }
}

void parse_start_line(const std::string& line, RtspMessage& message) {
    check_text(line);
    if (line.compare(0, 9, "RTSP/1.0 ") == 0) {
        // 版本后必须是三位状态码、一个空格、状态说明。
        if (line.size() < 13 || line[12] != ' ') {
            throw std::runtime_error("invalid RTSP response line");
        }
        message.is_request = false;
        message.status_code = static_cast<int>(parse_number(line.substr(9, 3), 999));
        if (message.status_code < 100) {
            throw std::runtime_error("invalid RTSP status code");
        }
        message.reason = line.substr(13);
        return;
    }

    const std::size_t first_space = line.find(' ');
    if (first_space == std::string::npos) {
        throw std::runtime_error("invalid RTSP request line");
    }
    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string::npos || line.substr(second_space + 1) != "RTSP/1.0") {
        throw std::runtime_error("invalid RTSP request line");
    }
    message.method = line.substr(0, first_space);
    message.uri = line.substr(first_space + 1, second_space - first_space - 1);
    if (!is_token(message.method) || message.uri.empty()
        || message.uri.find('\t') != std::string::npos) {
        throw std::runtime_error("invalid RTSP method or URI");
    }
}

}  // namespace

std::string RtspMessage::header(const std::string& name) const {
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (same_header_name(headers[i].name, name)) {
            return headers[i].value;
        }
    }
    return "";
}

void RtspMessage::set_header(const std::string& name, const std::string& value) {
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (same_header_name(headers[i].name, name)) {
            headers[i].value = value;
            return;
        }
    }
    RtspHeader field;
    field.name = name;
    field.value = value;
    headers.push_back(field);
}

bool parse_rtsp_message(const std::string& data, RtspMessage& message,
                        std::size_t& consumed) {
    consumed = 0;
    const std::size_t header_end = data.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        if (data.size() >= kMaxRtspHeaderSize) {
            throw std::runtime_error("RTSP headers are too large");
        }
        return false;
    }
    const std::size_t body_begin = header_end + 4;
    if (body_begin > kMaxRtspHeaderSize) {
        throw std::runtime_error("RTSP headers are too large");
    }

    // 先在临时对象里解析。只有整条消息完整后，才修改输出对象。
    RtspMessage parsed;
    const std::size_t first_end = data.find("\r\n");
    parse_start_line(data.substr(0, first_end), parsed);

    bool has_length = false;
    bool has_cseq = false;
    std::size_t body_size = 0;
    std::size_t position = first_end + 2;
    while (position < header_end) {
        const std::size_t line_end = data.find("\r\n", position);
        const std::string line = data.substr(position, line_end - position);
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || !is_token(line.substr(0, colon))) {
            throw std::runtime_error("invalid RTSP header line");
        }
        RtspHeader field;
        field.name = line.substr(0, colon);
        field.value = trim_spaces(line.substr(colon + 1));
        check_text(field.value);

        if (same_header_name(field.name, "Content-Length")) {
            // 拒绝重复长度，即使两个值相同也不接受，避免消息边界歧义。
            if (has_length) {
                throw std::runtime_error("duplicate Content-Length");
            }
            has_length = true;
            body_size = parse_number(field.value, kMaxRtspBodySize);
        }
        if (same_header_name(field.name, "CSeq")) {
            if (has_cseq) {
                throw std::runtime_error("duplicate CSeq");
            }
            has_cseq = true;
            parse_number(field.value, 2147483647);
        }
        parsed.headers.push_back(field);
        position = line_end + 2;
    }
    if (!has_cseq) {
        throw std::runtime_error("RTSP message is missing CSeq");
    }
    // 没有 Content-Length 时，消息体长度为 0，不能用连接关闭来猜测长度。
    if (data.size() - body_begin < body_size) {
        return false;
    }
    parsed.body = data.substr(body_begin, body_size);
    message = parsed;
    consumed = body_begin + body_size;
    return true;
}

std::string serialize_rtsp_message(const RtspMessage& message) {
    if (message.body.size() > kMaxRtspBodySize) {
        throw std::runtime_error("RTSP body is too large");
    }
    std::string data;
    if (message.is_request) {
        if (!is_token(message.method)) {
            throw std::runtime_error("invalid RTSP method");
        }
        data = message.method + " " + message.uri + " RTSP/1.0\r\n";
    } else {
        data = "RTSP/1.0 " + std::to_string(message.status_code) + " " + message.reason + "\r\n";
    }
    for (std::size_t i = 0; i < message.headers.size(); ++i) {
        const RtspHeader& field = message.headers[i];
        if (!is_token(field.name)) {
            throw std::runtime_error("invalid RTSP header name");
        }
        check_text(field.value);
        if (!same_header_name(field.name, "Content-Length")) {
            data += field.name + ": " + field.value + "\r\n";
        }
    }
    data += "Content-Length: " + std::to_string(message.body.size()) + "\r\n\r\n";
    data += message.body;

    // 复用解析检查，确保手工创建的消息也符合相同规则。
    // 起始行字段先检查，防止换行被误当成额外头字段。
    check_text(message.method);
    check_text(message.uri);
    check_text(message.reason);
    RtspMessage checked;
    std::size_t consumed = 0;
    if (!parse_rtsp_message(data, checked, consumed) || consumed != data.size()) {
        throw std::runtime_error("cannot serialize invalid RTSP message");
    }
    return data;
}

}  // namespace rtsp
