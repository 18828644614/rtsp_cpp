#include "media/h264_depacketizer.h"
#include "media/h264_packetizer.h"
#include "rtsp/rtsp_stream_parser.h"

#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

rtsp::RtpPacket make_packet(const std::string& payload, unsigned int sequence = 100) {
    rtsp::RtpPacket packet;
    packet.payload_type = 104;
    packet.sequence_number = static_cast<std::uint16_t>(sequence);
    packet.timestamp = 9000;
    packet.ssrc = 0x12345678;
    packet.payload = payload;
    return packet;
}

void expect_bad_nalu(const std::string& nalu, std::size_t limit = 1200) {
    rtsp::H264Packetizer packetizer;
    bool rejected = false;
    try {
        packetizer.packetize(nalu, limit);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "invalid packetizer input was accepted");
}

void expect_bad_payload(rtsp::H264Depacketizer& depacketizer, const std::string& payload) {
    std::vector<std::string> nalus(1, "old output");
    bool rejected = false;
    try {
        depacketizer.push(make_packet(payload), nalus);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "invalid H264 payload was accepted");
    check(nalus.empty(), "invalid payload leaked partial output");
    // 错误发生后，旧 FU-A 的结束片不能继续拼接成功。
    check(depacketizer.push(make_packet(std::string("\x7c\x45", 2) + "end", 101), nalus)
              == rtsp::H264Dropped, "malformed payload did not reset assembly");
}

void test_fixed_payloads() {
    rtsp::H264Packetizer packetizer;
    rtsp::H264Depacketizer depacketizer;
    std::vector<std::string> nalus;
    const std::string nalu = std::string("\x65", 1) + "abcdef";

    std::vector<std::string> payloads = packetizer.packetize(nalu, nalu.size());
    check(payloads.size() == 1 && payloads[0] == nalu, "single NALU boundary failed");
    check(depacketizer.push(make_packet(payloads[0]), nalus) == rtsp::H264Complete
          && nalus.size() == 1 && nalus[0] == nalu, "single NALU changed");

    // 用手写字节检验 FU-A：原始头 0x65 → indicator 0x7c，类型仍为 5。
    payloads = packetizer.packetize(nalu, 4);
    check(payloads.size() == 3, "FU-A packet count failed");
    check(payloads[0] == std::string("\x7c\x85", 2) + "ab", "FU-A start bytes failed");
    check(payloads[1] == std::string("\x7c\x05", 2) + "cd", "FU-A middle bytes failed");
    check(payloads[2] == std::string("\x7c\x45", 2) + "ef", "FU-A end bytes failed");

    for (std::size_t i = 0; i < payloads.size(); ++i) {
        rtsp::RtpPacket packet = make_packet(payloads[i], 65535 + static_cast<unsigned int>(i));
        // Marker 故意放在开始片上；重组完整性只看 FU-A 的 S/E 位。
        packet.marker = i == 0;
        const rtsp::H264DepacketizeResult result = depacketizer.push(packet, nalus);
        if (i + 1 == payloads.size()) {
            check(result == rtsp::H264Complete && nalus.size() == 1 && nalus[0] == nalu,
                  "FU-A reassembly or sequence wrap failed");
        } else {
            check(result == rtsp::H264Waiting && nalus.empty(), "partial NALU was returned");
        }
    }

    // 大小恰好超过限制一个字节，以及最小负载上限 3。
    payloads = packetizer.packetize(nalu, nalu.size() - 1);
    check(payloads.size() == 2, "one byte over limit did not fragment");
    payloads = packetizer.packetize(nalu, 3);
    check(payloads.size() == 6, "minimum payload limit failed");
    check(packetizer.packetize(std::string("\x09", 1)).size() == 1, "one-byte NALU failed");

    // STAP-A 手写两个长度与 NALU，含零字节和高位字节。
    const std::string stap("\x78\x00\x03\x67\x00\xff\x00\x02\x68\x24", 10);
    check(depacketizer.push(make_packet(stap), nalus) == rtsp::H264Complete
          && nalus.size() == 2 && nalus[0] == std::string("\x67\x00\xff", 3)
          && nalus[1] == std::string("\x68\x24", 2), "fixed STAP-A failed");
}

void test_continuity_and_recovery() {
    const std::string start = std::string("\x7c\x85", 2) + "ab";
    const std::string middle = std::string("\x7c\x05", 2) + "cd";
    const std::string end = std::string("\x7c\x45", 2) + "ef";
    rtsp::H264Depacketizer depacketizer;
    std::vector<std::string> nalus;

    check(depacketizer.push(make_packet(middle), nalus) == rtsp::H264Dropped,
          "orphan middle fragment was accepted");
    check(depacketizer.push(make_packet(end), nalus) == rtsp::H264Dropped,
          "orphan end fragment was accepted");

    // 分别打破序号、时间戳、SSRC、PT、类型与 NRI，再验证新开始片能恢复。
    for (int scenario = 0; scenario < 7; ++scenario) {
        check(depacketizer.push(make_packet(start, 100), nalus) == rtsp::H264Waiting,
              "start fragment was rejected");
        rtsp::RtpPacket broken = make_packet(middle, 101);
        if (scenario == 0) {
            broken.sequence_number = 102;  // 缺片。
        } else if (scenario == 1) {
            broken.timestamp += 3600;
        } else if (scenario == 2) {
            broken.ssrc += 1;
        } else if (scenario == 3) {
            broken.payload_type += 1;
        } else if (scenario == 4) {
            broken.payload[1] = 1;  // 原始 NAL 类型变化。
        } else if (scenario == 5) {
            broken.payload[0] = 0x5c;  // NRI 变化。
        } else {
            broken.sequence_number = 100;  // 重复或倒退。
        }
        check(depacketizer.push(broken, nalus) == rtsp::H264Dropped && nalus.empty(),
              "broken continuity was accepted");
        check(depacketizer.push(make_packet(end, 102), nalus) == rtsp::H264Dropped,
              "discarded fragments became complete");
        depacketizer.push(make_packet(start, 200), nalus);
        check(depacketizer.push(make_packet(end, 201), nalus) == rtsp::H264Complete
              && nalus[0] == std::string("\x65", 1) + "abef", "recovery failed");
    }

    depacketizer.push(make_packet(start), nalus);
    depacketizer.reset();
    check(depacketizer.push(make_packet(end, 101), nalus) == rtsp::H264Dropped,
          "explicit reset failed");

    depacketizer.push(make_packet(start, 1), nalus);
    depacketizer.push(make_packet(start, 50), nalus);  // 新开始片替换旧分片。
    check(depacketizer.push(make_packet(end, 51), nalus) == rtsp::H264Complete,
          "new start did not replace old fragments");

    depacketizer.push(make_packet(start), nalus);
    check(depacketizer.push(make_packet(std::string("\x61", 1) + "single"), nalus)
              == rtsp::H264Complete, "single NALU did not interrupt old FU-A");
    check(depacketizer.push(make_packet(end, 101), nalus) == rtsp::H264Dropped,
          "old fragments survived a single NALU");

    // 接收端忽略 FU-A R 位，允许空分片，仍检查序号。
    depacketizer.push(make_packet(std::string("\x7c\xa5", 2), 20), nalus);
    check(depacketizer.push(make_packet(std::string("\x7c\x65", 2) + "x", 21), nalus)
              == rtsp::H264Complete && nalus[0] == std::string("\x65", 1) + "x",
          "reserved bit or empty fragment handling failed");
}

void test_invalid_payloads() {
    expect_bad_nalu("");
    expect_bad_nalu(std::string("\x65", 1), 2);
    const unsigned int bad_headers[] = {0, 24, 28, 31, 0xe5};
    for (std::size_t i = 0; i < 5; ++i) {
        expect_bad_nalu(std::string(1, static_cast<char>(bad_headers[i])));
    }
    expect_bad_nalu(std::string("\x00\x00\x00\x01\x65", 5));  // Annex-B 输入。

    rtsp::H264Depacketizer depacketizer;
    std::vector<std::string> nalus;
    const std::string start = std::string("\x7c\x85", 2) + "ab";
    const std::string bad[] = {
        "", std::string("\xe5", 1), std::string("\x00", 1),
        std::string("\x79", 1), std::string("\x7c", 1),
        std::string("\x7c\xc5", 2), std::string("\x7c\x80", 2),
        std::string("\x7c\x98", 2), std::string("\x78", 1),
        std::string("\x78\x00", 2), std::string("\x78\x00\x00", 3),
        std::string("\x78\x00\x02\x67", 4),
        std::string("\x78\x00\x01\x78", 4),  // 聚合内不能再嵌套 STAP-A。
        std::string("\x78\x00\x01\xe7", 4),
        std::string("\x78\x00\x01\x67\x00", 5)  // 第一条合法，后面截断。
    };
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        depacketizer.push(make_packet(start), nalus);
        expect_bad_payload(depacketizer, bad[i]);
    }

    // 最大 NALU 边界：先累计到恰好 4 MiB，再增加一字节应拒绝并复位。
    const std::size_t limit = 4 * 1024 * 1024;
    rtsp::RtpPacket packet = make_packet(std::string("\x7c\x85", 2) + std::string(limit - 1, 'a'));
    check(depacketizer.push(packet, nalus) == rtsp::H264Waiting, "maximum assembly failed");
    packet = make_packet(std::string("\x7c\x45", 2), 101);
    check(depacketizer.push(packet, nalus) == rtsp::H264Complete && nalus[0].size() == limit,
          "maximum NALU completion failed");
    depacketizer.push(make_packet(std::string("\x7c\x85", 2) + std::string(limit - 1, 'a')), nalus);
    expect_bad_payload(depacketizer, std::string("\x7c\x85", 2) + std::string(limit, 'a'));
    depacketizer.push(make_packet(std::string("\x7c\x85", 2) + std::string(limit - 1, 'a')), nalus);
    bool rejected = false;
    try {
        depacketizer.push(make_packet(std::string("\x7c\x45", 2) + "x", 101), nalus);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected && nalus.empty(), "assembly overflow was accepted");
    expect_bad_payload(depacketizer, std::string("\x65", 1) + std::string(limit, 'a'));
}

void test_interleaved_pipeline() {
    // 一个访问单元含 SPS、PPS 和一条较大的 IDR NALU。它们使用相同时间戳。
    std::vector<std::string> originals;
    originals.push_back(std::string("\x67\x00\xff", 3));
    originals.push_back(std::string("\x68\x24", 2));
    std::string idr(8001, '\0');
    idr[0] = 0x65;
    for (std::size_t i = 1; i < idr.size(); ++i) {
        idr[i] = static_cast<char>(i % 256);
    }
    originals.push_back(idr);

    rtsp::H264Packetizer packetizer;
    std::string wire;
    unsigned int sequence = 65534;
    unsigned int expected_packets = 0;
    for (std::size_t i = 0; i < originals.size(); ++i) {
        const std::vector<std::string> payloads = packetizer.packetize(originals[i]);
        for (std::size_t j = 0; j < payloads.size(); ++j) {
            check(payloads[j].size() <= 1200, "payload exceeded its limit");
            rtsp::RtpPacket packet = make_packet(payloads[j], sequence);
            ++sequence;
            ++expected_packets;
            packet.marker = i + 1 == originals.size() && j + 1 == payloads.size();
            wire += rtsp::serialize_interleaved_frame(6, rtsp::serialize_rtp_packet(packet));
        }
    }
    // 混入 RTSP 响应与 RTCP 通道，验证它们不会送给 H.264 解包器。
    wire += "RTSP/1.0 200 OK\r\nCSeq: 9\r\nContent-Length: 0\r\n\r\n";
    wire += rtsp::serialize_interleaved_frame(7, std::string("\x80\xc9\x00\x01", 4));

    rtsp::RtspStreamParser parser;
    rtsp::RtspStreamItem item;
    rtsp::H264Depacketizer depacketizer;
    std::vector<std::string> recovered;
    std::vector<std::string> nalus;
    unsigned int packet_count = 0;
    unsigned int marker_count = 0;
    unsigned int control_count = 0;
    unsigned int rtcp_count = 0;
    // 刻意逐字节输入，让交织头和 RTP 数据经历任意 TCP 半包。
    for (std::size_t i = 0; i < wire.size(); ++i) {
        parser.append(wire.data() + i, 1);
        while (parser.next(item)) {
            if (!item.is_interleaved) {
                ++control_count;
                check(item.message.header("CSeq") == "9", "mixed control response changed");
            } else if (item.channel == 7) {
                ++rtcp_count;  // 这里只验证分流，不解析 RTCP。
            } else {
                check(item.channel == 6, "unexpected channel");
                const rtsp::RtpPacket packet = rtsp::parse_rtp_packet(item.payload);
                check(packet.payload_type == 104 && packet.timestamp == 9000
                      && packet.ssrc == 0x12345678, "pipeline RTP metadata changed");
                check(packet.sequence_number == static_cast<std::uint16_t>(65534 + packet_count),
                      "pipeline sequence changed");
                ++packet_count;
                if (packet.marker) {
                    ++marker_count;
                    check(packet_count == expected_packets, "marker was set before the final packet");
                }
                const rtsp::H264DepacketizeResult result = depacketizer.push(packet, nalus);
                check(result != rtsp::H264Dropped, "pipeline dropped a valid fragment");
                for (std::size_t j = 0; j < nalus.size(); ++j) {
                    recovered.push_back(nalus[j]);
                }
            }
        }
    }
    parser.finish();
    check(recovered == originals, "end-to-end NALUs differ from originals");
    check(packet_count == expected_packets && marker_count == 1
          && control_count == 1 && rtcp_count == 1, "pipeline routing counts failed");
}

}  // namespace

int main() {
    try {
        test_fixed_payloads();
        std::cout << "PASS: fixed single NALU, FU-A and STAP-A bytes\n";
        test_continuity_and_recovery();
        std::cout << "PASS: sequence wrap, loss detection, reset and recovery\n";
        test_invalid_payloads();
        std::cout << "PASS: malformed payloads and NALU size limits\n";
        test_interleaved_pipeline();
        std::cout << "PASS: H264 / RTP / interleaved frames memory pipeline\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
