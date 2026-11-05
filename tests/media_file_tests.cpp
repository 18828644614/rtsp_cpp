#include "app/command_line.h"
#include "media/file_video_source.h"
#include "media/h264_file_reader.h"
#include "media/h264_file_writer.h"
#include "media/h264_depacketizer.h"
#include "sdp/session_description.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
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
    check(frame.is_keyframe && frame.pts == 0 && frame.duration == 3600,
          "IDR metadata was not populated");
    const std::vector<std::string> first = frame.nalus;
    for (std::size_t index = 0; index < frame.nalus.size(); ++index) {
        writer.write(frame.nalus[index]);
    }
    check(reader.read(frame) && frame.nalus.size() == 2, "final access unit missing");
    check(!frame.is_keyframe && frame.pts == 3600 && frame.duration == 3600,
          "non-IDR inherited keyframe flag or timestamp");
    const std::vector<std::string> second = frame.nalus;
    for (std::size_t index = 0; index < frame.nalus.size(); ++index) {
        writer.write(frame.nalus[index]);
    }
    check(!reader.read(frame) && frame.nalus.empty(), "EOF did not clear output");
    check(!frame.is_keyframe && frame.pts == 0 && frame.duration == 0,
          "EOF did not clear metadata");
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

void test_file_video_source_contract() {
    const std::string aud("\x09\xf0", 2);
    const std::string sps("\x67\x64\0\x1f", 4);
    const std::string pps("\x68\xee", 2);
    const std::string idr("\x65\xab", 2);
    const std::string updated_sps("\x67\x64\0\x20", 4);
    const std::string updated_pps("\x68\xef", 2);
    save(annex_b(aud) + annex_b(sps) + annex_b(pps) + annex_b(idr)
         + annex_b(aud) + annex_b(idr)
         + annex_b(aud) + annex_b(updated_sps) + annex_b(updated_pps) + annex_b(idr)
         + annex_b(aud) + annex_b(idr));
    std::unique_ptr<rtsp::VideoSource> source = std::make_unique<rtsp::FileVideoSource>(input_path);
    check(std::remove(input_path.c_str()) == 0, "file video source kept the input open");
    const auto& track = source->track();
    check(track.media_type == "video" && track.encoding == "H264" && track.clock_rate == 90000
              && track.payload_type == 96 && track.control == "trackID=0"
              && track.fmtp == rtsp::make_h264_fmtp(sps, pps), "file video track changed");
    rtsp::H264AccessUnit frame;
    for (int index = 0; index < 4; ++index) {
        check(!source->ended() && source->read(frame), "file video source ended early");
        const auto& expected_sps = index < 2 ? sps : updated_sps;
        const auto& expected_pps = index < 2 ? pps : updated_pps;
        check(frame.nalus == std::vector<std::string>({aud, expected_sps, expected_pps, idr}),
              "IDR parameters did not follow playback order");
        check(frame.is_keyframe && frame.pts == static_cast<std::uint64_t>(index) * 3600
                  && frame.duration == 3600, "file video timing changed");
    }
    check(source->ended(), "last file AU did not mark source ended");
    check(!source->read(frame) && frame.nalus.empty() && !frame.is_keyframe
              && frame.pts == 0 && frame.duration == 0 && source->ended(),
          "file source EOF did not clear output or remain ended");
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
        test_file_video_source_contract();
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
