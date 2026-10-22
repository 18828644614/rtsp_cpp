#include "app/command_line.h"
#include "media/h264_file_reader.h"
#include "media/h264_file_writer.h"
#include "media/h264_depacketizer.h"
#include "sdp/session_description.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

const std::string input_path = "media_test_input.h264";
const std::string output_path = "media_test_output.h264";

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void save(const std::string& data) {
    std::ofstream output(input_path, std::ios::binary);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    check(static_cast<bool>(output), "failed to prepare test input");
}

std::string annex_b(const std::string& nalu, bool short_prefix = false) {
    const std::string prefix = short_prefix ? std::string("\0\0\1", 3) : std::string("\0\0\0\1", 4);
    return prefix + nalu;
}

std::string sample() {
    return annex_b(std::string("\x09\xf0", 2))
           + annex_b(std::string("\x67\x64\0\x1f", 4), true)
           + annex_b(std::string("\x68\xee", 2))
           + annex_b(std::string("\x65", 1) + std::string(4000, 'x'), true)
           + annex_b(std::string("\x65\xab", 2))
           + annex_b(std::string("\x09\xf0", 2), true)
           + annex_b(std::string("\x41\xcd", 2));
}

void test_round_trip() {
    save(std::string(2, '\0') + sample() + std::string(3, '\0'));
    rtsp::H264FileReader reader;
    reader.open(input_path);
    check(reader.frame_count() == 2, "AUD grouping failed");
    check(reader.sps() == std::string("\x67\x64\0\x1f", 4), "SPS changed");
    check(reader.pps() == std::string("\x68\xee", 2), "PPS changed");
    rtsp::H264FileWriter writer;
    writer.open(output_path);
    rtsp::H264AccessUnit frame;
    check(reader.read(frame) && frame.nalus.size() == 5, "multi-slice access unit split");
    const std::vector<std::string> first = frame.nalus;
    for (std::size_t index = 0; index < frame.nalus.size(); ++index) {
        writer.write(frame.nalus[index]);
    }
    check(reader.read(frame) && frame.nalus.size() == 2, "final access unit missing");
    const std::vector<std::string> second = frame.nalus;
    for (std::size_t index = 0; index < frame.nalus.size(); ++index) {
        writer.write(frame.nalus[index]);
    }
    check(!reader.read(frame) && frame.nalus.empty(), "EOF did not clear output");
    writer.close();
    writer.close();
    reader.open(output_path);
    check(reader.read(frame) && frame.nalus == first, "first access unit round trip changed");
    check(reader.read(frame) && frame.nalus == second, "second access unit round trip changed");
    check(!reader.read(frame), "round trip added a picture");
}

void test_invalid_files() {
    const std::vector<std::string> invalid = {
        "", "not Annex-B", annex_b(""), annex_b(std::string("\x65\xaa", 2)),
        annex_b(std::string("\x09\xf0", 2)) + annex_b(std::string("\x65\xaa", 2)),
        sample() + annex_b(""),
        sample() + annex_b(std::string("\x09\xf0", 2)),
        sample() + annex_b(std::string("\x89\xf0", 2)),
        sample() + annex_b(std::string("\x65", 1) + std::string(4 * 1024 * 1024, 'x'))
    };
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        save(invalid[index]);
        rtsp::H264FileReader reader;
        bool rejected = false;
        try {
            reader.open(input_path);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected && reader.frame_count() == 0, "invalid input was accepted");
    }
    rtsp::H264FileWriter writer;
    bool rejected = false;
    try {
        writer.write("x");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "write before open succeeded");
}

void test_parameters_and_finish() {
    const std::string fmtp = rtsp::make_h264_fmtp(
        std::string("\x67\x64\0\x1f", 4), std::string("\x68\xee", 2));
    check(fmtp == "packetization-mode=1;profile-level-id=64001f;sprop-parameter-sets=Z2QAHw==,aO4=",
          "SDP base64 or profile is wrong");
    rtsp::H264Depacketizer depacketizer;
    rtsp::RtpPacket packet;
    packet.payload = std::string("\x7c\x85\xaa", 3);
    std::vector<std::string> nalus;
    check(depacketizer.push(packet, nalus) == rtsp::H264Waiting, "FU-A start failed");
    bool rejected = false;
    try {
        depacketizer.finish();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "EOF accepted a partial FU-A");
    depacketizer.reset();
    depacketizer.finish();
    check(rtsp::app::parse_frame_limit("0") == 0
          && rtsp::app::parse_frame_limit("1000000") == 1000000, "valid frame limit rejected");
    for (const char* text : {"", "-1", "1x", "1000001", "9999999999999999999"}) {
        rejected = false;
        try {
            rtsp::app::parse_frame_limit(text);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "invalid frame limit accepted");
    }
}

}

int main() {
    try {
        test_round_trip();
        test_invalid_files();
        test_parameters_and_finish();
        std::remove(input_path.c_str());
        std::remove(output_path.c_str());
        std::cout << "PASS: H264 files, AUD grouping, SDP parameters, EOF and arguments\n";
        return 0;
    } catch (const std::exception& error) {
        std::remove(input_path.c_str());
        std::remove(output_path.c_str());
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
