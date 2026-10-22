#include "rtp/rtp_packet.h"

#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_bad_input(const std::string& wire) {
    bool rejected = false;
    try {
        rtsp::parse_rtp_packet(wire);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "invalid RTP input was accepted");
}

void expect_bad_output(const rtsp::RtpPacket& packet) {
    bool rejected = false;
    try {
        rtsp::serialize_rtp_packet(packet);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "invalid RTP output was accepted");
}

void test_fixed_bytes() {
    // 手写线上的字节：V=2、M=1、PT=104、序号=0x1234。
    // 不先调用自己的序列化器，避免解析和生成出现相同错误却互相通过。
    const std::string header("\x80\xe8\x12\x34\x01\x02\x03\x04\xaa\xbb\xcc\xdd", 12);
    const std::string payload("\x65\x00\x24\xff", 4);
    const std::string wire = header + payload;
    const rtsp::RtpPacket parsed = rtsp::parse_rtp_packet(wire);
    check(parsed.marker && parsed.payload_type == 104, "marker or payload type changed");
    check(parsed.sequence_number == 0x1234, "sequence byte order changed");
    check(parsed.timestamp == 0x01020304 && parsed.ssrc == 0xaabbccdd,
          "timestamp or SSRC byte order changed");
    check(parsed.csrcs.empty() && !parsed.has_extension && parsed.padding_size == 0,
          "absent optional fields became present");
    check(parsed.payload == payload, "binary payload changed");
    check(rtsp::serialize_rtp_packet(parsed) == wire, "fixed bytes were not reproduced");

    // 独立从字段生成，检查最大值不会受到符号或字节序影响。
    rtsp::RtpPacket packet;
    packet.payload_type = 127;
    packet.sequence_number = 65535;
    packet.timestamp = 0xffffffff;
    packet.ssrc = 0xffffffff;
    const std::string maximum("\x80\x7f\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 12);
    check(rtsp::serialize_rtp_packet(packet) == maximum, "maximum fields failed");
    check(rtsp::parse_rtp_packet(maximum).payload.empty(), "empty RTP payload failed");
}

void test_optional_fields() {
    // V=2、P=1、X=1、CC=2；扩展长度=1 个 32 位字；最后 4 字节为 Padding。
    const std::string wire = std::string(
        "\xb2\x60\x00\x01\x00\x00\x0e\x10\x11\x22\x33\x44"
        "\x01\x02\x03\x04\xff\xee\xdd\xcc"
        "\xbe\xde\x00\x01\x00\xff\x24\x80"
        "\x61\x00\x24\x00\x00\x00\x04", 35);
    const rtsp::RtpPacket parsed = rtsp::parse_rtp_packet(wire);
    check(parsed.csrcs.size() == 2 && parsed.csrcs[0] == 0x01020304
          && parsed.csrcs[1] == 0xffeeddcc, "CSRC list failed");
    check(parsed.has_extension && parsed.extension_profile == 0xbede,
          "extension profile failed");
    check(parsed.extension_data == std::string("\x00\xff\x24\x80", 4),
          "extension length was not interpreted in words");
    check(parsed.padding_size == 4 && parsed.payload == std::string("\x61\x00\x24", 3),
          "padding was not removed from the payload");
    check(rtsp::serialize_rtp_packet(parsed) == wire, "optional fields serialization failed");

    rtsp::RtpPacket packet;
    packet.has_extension = true;  // 长度为 0 的扩展是合法的。
    packet.extension_profile = 65535;
    packet.padding_size = 255;
    packet.csrcs.assign(15, 0xffffffff);
    packet.payload = "abc";
    rtsp::RtpPacket again = rtsp::parse_rtp_packet(rtsp::serialize_rtp_packet(packet));
    check(again.csrcs == packet.csrcs && again.has_extension && again.extension_data.empty()
          && again.extension_profile == 65535 && again.padding_size == 255
          && again.payload == "abc", "optional field boundaries failed");

    packet.padding_size = 1;
    packet.payload.clear();
    again = rtsp::parse_rtp_packet(rtsp::serialize_rtp_packet(packet));
    check(again.padding_size == 1 && again.payload.empty(), "padding-only packet failed");
}

void test_bad_input() {
    const std::string header("\x80\x60\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00", 12);
    for (std::size_t i = 0; i < header.size(); ++i) {
        expect_bad_input(header.substr(0, i));
    }
    std::string wire = header;
    wire[0] = 0x40;
    expect_bad_input(wire);  // 版本不是 2。

    wire = header;
    wire[0] = static_cast<char>(0x82);
    for (std::size_t i = 0; i < 8; ++i) {
        expect_bad_input(wire + std::string(i, '\0'));
    }

    wire = header;
    wire[0] = static_cast<char>(0x90);
    for (std::size_t i = 0; i < 4; ++i) {
        expect_bad_input(wire + std::string(i, '\0'));
    }
    wire += std::string("\xbe\xde\x00\x02", 4);
    for (std::size_t i = 0; i < 8; ++i) {
        expect_bad_input(wire + std::string(i, '\0'));
    }

    wire = header;
    wire[0] = static_cast<char>(0xa0);
    expect_bad_input(wire);  // P=1 却没有任何 Padding。
    expect_bad_input(wire + std::string("\0", 1));
    expect_bad_input(wire + std::string("\x02", 1));

    // Padding 不能跨过已解析的 CSRC 或扩展头。
    wire = header;
    wire[0] = static_cast<char>(0xb1);
    wire += std::string(4, '\0');
    wire += std::string("\xbe\xde\x00\x01\x00\x00\x00\x00", 8);
    expect_bad_input(wire + std::string("\x02", 1));
}

void test_bad_output() {
    rtsp::RtpPacket packet;
    packet.payload_type = 128;
    expect_bad_output(packet);
    packet.payload_type = 96;
    packet.csrcs.assign(16, 0);
    expect_bad_output(packet);
    packet.csrcs.clear();
    packet.padding_size = 256;
    expect_bad_output(packet);
    packet.padding_size = 0;
    packet.extension_data = "abcd";
    expect_bad_output(packet);  // 没有设置 X 位。
    packet.extension_data.clear();
    packet.extension_profile = 1;
    expect_bad_output(packet);
    packet.has_extension = true;
    packet.extension_profile = 65536;
    expect_bad_output(packet);
    packet.extension_profile = 0;
    packet.extension_data = "abc";
    expect_bad_output(packet);  // 不是 4 字节的整数倍。
    packet.extension_data.assign(65536 * 4, '\0');
    expect_bad_output(packet);  // 长度字段装不下。

    packet.extension_data.assign(65535 * 4, '\0');
    const rtsp::RtpPacket maximum = rtsp::parse_rtp_packet(rtsp::serialize_rtp_packet(packet));
    check(maximum.extension_data == packet.extension_data, "maximum extension size failed");
}

}  // namespace

int main() {
    try {
        test_fixed_bytes();
        std::cout << "PASS: fixed RTP bytes and network byte order\n";
        test_optional_fields();
        std::cout << "PASS: CSRC, extension and padding\n";
        test_bad_input();
        test_bad_output();
        std::cout << "PASS: truncated input and invalid output fields\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
