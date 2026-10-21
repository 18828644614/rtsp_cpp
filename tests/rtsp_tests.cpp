#include "rtsp/rtsp_message.h"
#include "rtsp/rtsp_stream_parser.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_bad_message(const std::string& wire) {
    rtsp::RtspMessage message;
    std::size_t consumed = 0;
    bool rejected = false;
    try {
        rtsp::parse_rtsp_message(wire, message, consumed);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "invalid RTSP message was accepted");
    check(consumed == 0, "invalid message consumed bytes");
}

void expect_bad_output(const rtsp::RtspMessage& message) {
    bool rejected = false;
    try {
        rtsp::serialize_rtsp_message(message);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "invalid outgoing RTSP message was accepted");
}

void expect_incomplete_eof(const std::string& wire) {
    rtsp::RtspStreamParser parser;
    parser.append(wire.data(), wire.size());
    rtsp::RtspStreamItem item;
    check(!parser.next(item), "incomplete message was returned");
    bool rejected = false;
    try {
        parser.finish();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "truncated input was ignored at EOF");
}

void test_messages() {
    rtsp::RtspMessage request;
    request.method = "DESCRIBE";
    request.uri = "rtsp://127.0.0.1:8554/live";
    request.set_header("CSeq", "3");
    request.set_header("Accept", "application/sdp");
    request.set_header("cseq", "4");
    check(request.headers.size() == 2, "set_header duplicated a field");
    check(request.header("CSEQ") == "4", "header lookup is not case insensitive");

    const std::string wire = rtsp::serialize_rtsp_message(request);
    rtsp::RtspMessage parsed;
    std::size_t consumed = 0;
    check(rtsp::parse_rtsp_message(wire + "next message", parsed, consumed), "request failed");
    check(consumed == wire.size(), "parser consumed bytes from the next message");
    check(parsed.is_request && parsed.method == "DESCRIBE", "request method changed");
    check(parsed.uri == request.uri && parsed.header("CSeq") == "4", "request fields changed");
    check(parsed.body.empty(), "empty body changed");

    rtsp::RtspMessage response;
    response.is_request = false;
    response.status_code = 404;
    response.reason = "Not Found";
    response.set_header("CSeq", "4");
    response.set_header("Content-Length", "999");  // 故意写错，生成时必须重新计算。
    response.body = std::string("a\0$\r\nb", 6);
    const std::string response_wire = rtsp::serialize_rtsp_message(response);
    check(rtsp::parse_rtsp_message(response_wire, parsed, consumed), "response failed");
    check(!parsed.is_request && parsed.status_code == 404, "response status changed");
    check(parsed.reason == "Not Found", "response reason lost spaces");
    check(parsed.body == response.body, "binary body changed");
    check(parsed.header("Content-Length") == "6", "Content-Length was not recalculated");

    check(rtsp::parse_rtsp_message("OPTIONS * RTSP/1.0\r\ncSeQ:\t7 \t\r\n\r\n", parsed, consumed),
          "message without Content-Length failed");
    check(parsed.uri == "*" && parsed.header("CSeq") == "7", "header whitespace failed");
    check(parsed.body.empty(), "absent Content-Length did not mean an empty body");

    // 消息体少一个字节时不能输出消息，也不能修改调用方原有结果。
    parsed.method = "unchanged";
    consumed = 99;
    check(!rtsp::parse_rtsp_message(response_wire.substr(0, response_wire.size() - 1),
                                   parsed, consumed), "partial body was accepted");
    check(consumed == 0 && parsed.method == "unchanged", "incomplete parse changed output");
}

void test_invalid_messages() {
    const std::string bad_messages[] = {
        "OPTIONS * RTSP/2.0\r\nCSeq: 1\r\n\r\n",
        "OPTIONS  RTSP/1.0\r\nCSeq: 1\r\n\r\n",
        "OP(TIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: -1\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 2147483648\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\ncseq: 1\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nBad header: x\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nBroken\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: -1\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 1x\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: \r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 1\r\ncontent-length: 1\r\n\r\nx",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 999999999999999999999\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 1048577\r\n\r\n",
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\nPublic: OPTIONS\r\n\r\n",
        "RTSP/1.0 20 OK\r\nCSeq: 1\r\n\r\n",
        "RTSP/1.0 099 Invalid\r\nCSeq: 1\r\n\r\n",
        "RTSP/1.0 abc Invalid\r\nCSeq: 1\r\n\r\n"
    };
    for (std::size_t i = 0; i < sizeof(bad_messages) / sizeof(bad_messages[0]); ++i) {
        expect_bad_message(bad_messages[i]);
    }
    expect_bad_message(std::string(rtsp::kMaxRtspHeaderSize, 'x'));
    expect_bad_message("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nX: "
                       + std::string(rtsp::kMaxRtspHeaderSize, 'x') + "\r\n\r\n");

    rtsp::RtspMessage message;
    message.method = "OPTIONS";
    message.uri = "*";
    expect_bad_output(message);  // 缺少 CSeq。
    message.set_header("CSeq", "1");
    message.set_header("X-Test", "ok\r\nInjected: value");
    expect_bad_output(message);
    message.set_header("X-Test", "ok");
    message.uri = "*\r\nInjected: value";
    expect_bad_output(message);
    message.uri = "*";
    message.method = "RTSP/1.0";
    expect_bad_output(message);
}

// 混合流固定包含四项，按顺序检查内容，避免只检查消息总数。
void read_mixed_items(rtsp::RtspStreamParser& parser, int& count,
                      const std::string& payload, const std::string& body) {
    rtsp::RtspStreamItem item;
    while (parser.next(item)) {
        if (count == 0) {
            check(!item.is_interleaved && item.message.is_request, "first item is not a request");
            check(item.message.header("CSeq") == "1", "request CSeq changed");
        } else if (count == 1) {
            check(item.is_interleaved && item.channel == 200, "frame channel changed");
            check(item.payload == payload, "binary frame payload changed");
        } else if (count == 2) {
            check(!item.is_interleaved && !item.message.is_request, "response was misclassified");
            check(item.message.body == body, "RTSP body was split at a false boundary");
            check(item.payload.empty(), "old frame payload leaked into text message");
        } else if (count == 3) {
            check(item.is_interleaved && item.channel == 1, "empty frame was lost");
            check(item.payload.empty(), "empty frame has data");
        } else {
            check(false, "parser invented an extra item");
        }
        ++count;
    }
}

void test_stream_splits() {
    const std::string request = "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    std::string payload(300, '\0');  // 长度超过 255，验证交织帧长度的高位字节。
    payload.replace(10, 9, "$\r\n\r\nRTSP");

    rtsp::RtspMessage response;
    response.is_request = false;
    response.set_header("CSeq", "2");
    response.set_header("Content-Type", "application/sdp");
    response.body = "v=0\r\n\r\n$inside-body";
    response.body += '\0';
    const std::string mixed = request + rtsp::serialize_interleaved_frame(200, payload)
        + rtsp::serialize_rtsp_message(response) + rtsp::serialize_interleaved_frame(1, "");

    // 遍历每一个分割点，覆盖起始行、空行、消息体和交织帧的任意半包。
    // split=0 和 split=size 同时验证所有消息粘在一起的情况。
    for (std::size_t split = 0; split <= mixed.size(); ++split) {
        rtsp::RtspStreamParser parser;
        int count = 0;
        parser.append(mixed.data(), split);
        read_mixed_items(parser, count, payload, response.body);
        parser.append(mixed.data() + split, mixed.size() - split);
        read_mixed_items(parser, count, payload, response.body);
        check(count == 4, "split input lost an item");
        parser.finish();
    }

    rtsp::RtspStreamParser parser;
    int count = 0;
    for (std::size_t i = 0; i < mixed.size(); ++i) {
        parser.append(mixed.data() + i, 1);
        read_mixed_items(parser, count, payload, response.body);
    }
    check(count == 4, "one-byte input lost an item");
    parser.finish();
}

void test_limits_and_eof() {
    expect_incomplete_eof("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n");
    expect_incomplete_eof("RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 3\r\n\r\nab");
    expect_incomplete_eof("$");
    expect_incomplete_eof(std::string("$\0\0\3ab", 6));

    rtsp::RtspStreamParser parser;
    parser.append(nullptr, 0);
    parser.finish();
    rtsp::RtspStreamItem item;
    item.channel = 42;
    check(!parser.next(item) && item.channel == 42, "empty parser changed the output");

    const std::string payload(65535, 'x');
    const std::string frame = rtsp::serialize_interleaved_frame(255, payload);
    parser.append(frame.data(), frame.size());
    check(parser.next(item) && item.channel == 255 && item.payload == payload,
          "maximum interleaved frame failed");
    parser.finish();

    bool rejected = false;
    try {
        rtsp::serialize_interleaved_frame(256, "");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "out-of-range channel was accepted");
    rejected = false;
    try {
        rtsp::serialize_interleaved_frame(0, std::string(65536, 'x'));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "oversized frame was accepted");
    rejected = false;
    try {
        parser.append(nullptr, 1);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "null stream input was accepted");
    rejected = false;
    const std::string too_large(2 * 1024 * 1024 + 1, 'x');
    try {
        parser.append(too_large.data(), too_large.size());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "oversized stream buffer was accepted");

    rtsp::RtspMessage response;
    response.is_request = false;
    response.set_header("CSeq", "1");
    response.body.assign(rtsp::kMaxRtspBodySize, 'x');
    const std::string wire = rtsp::serialize_rtsp_message(response);
    parser.append(wire.data(), wire.size());
    check(parser.next(item) && item.message.body == response.body, "maximum RTSP body failed");
    parser.finish();
    response.body += 'x';
    expect_bad_output(response);
}

}  // namespace

int main() {
    try {
        test_messages();
        std::cout << "PASS: request/response, headers, binary body and serialization\n";
        test_invalid_messages();
        std::cout << "PASS: malformed RTSP and unsafe outgoing messages\n";
        test_stream_splits();
        std::cout << "PASS: every split point, one-byte input and mixed interleaved frames\n";
        test_limits_and_eof();
        std::cout << "PASS: size limits, truncated input and EOF\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
