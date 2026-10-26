#include "media/aac_file_reader.h"
#include "media/aac_packetizer.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

const std::string path = "aac_test.aac";

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void save(const std::string& data) {
    std::ofstream output(path, std::ios::binary);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    check(static_cast<bool>(output), "failed to write AAC fixture");
}

// 手写 ADTS：AAC-LC、48 kHz、双声道，AU 为 11 22 33。
const std::string sample("\xff\xf1\x4c\x80\x01\x5f\xfc\x11\x22\x33", 10);

void test_reader_and_payload() {
    save(sample + sample);
    rtsp::AacFileReader reader;
    reader.open(path);
    check(std::remove(path.c_str()) == 0, "AAC reader kept file open");
    const rtsp::MediaTrack& track = reader.track();
    check(track.media_type == "audio" && track.encoding == "MPEG4-GENERIC"
              && track.clock_rate == 48000 && track.channels == 2, "wrong ADTS configuration");
    check(track.fmtp == "streamtype=5;profile-level-id=1;mode=AAC-hbr;config=1190;"
                       "SizeLength=13;IndexLength=3;IndexDeltaLength=3", "wrong AAC SDP config");
    rtsp::AacAccessUnit frame;
    check(reader.read(frame) && frame.pts == 0 && frame.duration == 1024
              && frame.data == std::string("\x11\x22\x33", 3), "ADTS header was not removed");
    const auto payload = rtsp::AacPacketizer().packetize(frame, track.clock_rate);
    check(payload->payloads.size() == 1
              && payload->payloads[0] == std::string("\0\x10\0\x18\x11\x22\x33", 7),
          "AAC-hbr fixed payload bytes do not match RFC layout");
    check(reader.read(frame) && frame.pts == 1024 && reader.ended(), "AAC PTS or EOF is wrong");
    check(!reader.read(frame) && frame.data.empty(), "AAC EOF returned stale data");

    // CRC 头为 9 字节；第一版跳过 CRC 字段，不执行校验。
    std::string crc = sample;
    crc[1] = static_cast<char>(0xf0);
    crc[5] = static_cast<char>(0x9f);  // 总长从 10 改为 12。
    crc.insert(7, std::string("\xab\xcd", 2));
    save(crc);
    reader.open(path);
    check(reader.read(frame) && frame.data == std::string("\x11\x22\x33", 3), "CRC header leaked");

    std::string mono = sample;
    mono[2] = static_cast<char>(0x50);  // AAC-LC、44.1 kHz。
    mono[3] = static_cast<char>(0x40);  // 单声道。
    save(mono);
    reader.open(path);
    check(reader.track().clock_rate == 44100 && reader.track().channels == 1
              && reader.track().fmtp.find("config=1208") != std::string::npos,
          "44.1 kHz mono config was not parsed");
}

void reject(const std::string& data) {
    save(data);
    bool rejected = false;
    try {
        rtsp::AacFileReader reader;
        reader.open(path);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "invalid ADTS input was accepted");
}

void test_invalid_input() {
    reject("");
    reject(sample.substr(0, 6));
    reject(sample.substr(0, 9));
    reject(sample + "x");
    std::string bad = sample;
    bad[0] = 0;
    reject(bad);
    bad = sample;
    bad[1] = static_cast<char>(0xf3);  // 非零 layer。
    reject(bad);
    bad = sample;
    bad[2] = static_cast<char>(0x0c);  // AAC Main。
    reject(bad);
    bad = sample;
    bad[2] = static_cast<char>(0x7c);  // 保留采样率索引。
    reject(bad);
    bad = sample;
    bad[3] = 0;                       // 声道配置为 0，需要解析 PCE。
    reject(bad);
    bad = sample;
    bad[6] = static_cast<char>(0xfd);  // 多个 raw data block。
    reject(bad);
    bad = sample;
    bad[2] = static_cast<char>(0x50);  // 文件中途改变采样率。
    reject(sample + bad);
    bad = sample;
    bad[3] = static_cast<char>(0x40);
    reject(sample + bad);

    rtsp::AacAccessUnit oversized;
    oversized.data.assign(8192, 'x');
    bool rejected = false;
    try {
        rtsp::AacPacketizer().packetize(oversized, 48000);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "13-bit AAC AU size limit was ignored");
}

}  // namespace

int main() {
    try {
        test_reader_and_payload();
        test_invalid_input();
        std::remove(path.c_str());
        std::cout << "PASS: ADTS, CRC header, AAC config, AU headers, sample clock and invalid input\n";
        return 0;
    } catch (const std::exception& error) {
        std::remove(path.c_str());
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
