#include "app/command_line.h"
#include "app/synthetic_audio_input.h"
#include "media/aac_encoder.h"
#ifdef RTSP_HAS_MICROPHONE_CAPTURE
#include "media/microphone_capture_input.h"
#endif

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace {

std::atomic<bool> stopped{false};
BOOL WINAPI console_control(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) { return FALSE; }
    stopped.store(true);
    return TRUE;
}

std::string utf8(const wchar_t* value) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) { throw std::invalid_argument("invalid Unicode argument"); }
    std::string text(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, text.data(), count, nullptr, nullptr);
    text.pop_back();
    return text;
}

void write_adts(std::ofstream& file, const rtsp::AacAccessUnit& frame) {
    const auto size = frame.data.size() + 7;
    const unsigned char header[] = {0xff, 0xf1, 0x4c,
        static_cast<unsigned char>(0x40 | (size >> 11)),
        static_cast<unsigned char>(size >> 3),
        static_cast<unsigned char>(((size & 7) << 5) | 0x1f), 0xfc};
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(frame.data.data(), static_cast<std::streamsize>(frame.data.size()));
}

}

int wmain(int argc, wchar_t* argv[]) {
    try {
        SetConsoleOutputCP(CP_UTF8);
        bool synthetic = false;
        int seconds = 2;
        int device_index = -1;
        std::string device;
        std::string output = "audio-probe.aac";
        std::set<std::string> seen;
        for (int index = 1; index < argc; ++index) {
            const auto option = utf8(argv[index]);
            if (option == "--help" && argc == 2) {
                std::cout << "Usage: aac_encode_probe --synthetic | --device NAME | --device-index N\n"
                          << "  --seconds N (1..3600, default 2) --output FILE (default audio-probe.aac)\n"
                          << "Writes ADTS, FILE.csv timestamps, and FILE.json padding evidence.\n";
                return 0;
            }
            if (!seen.insert(option).second) { throw std::invalid_argument("duplicate option: " + option); }
            if (option == "--synthetic") { synthetic = true; continue; }
            if (option != "--seconds" && option != "--device" && option != "--device-index" && option != "--output") {
                throw std::invalid_argument("unknown option: " + option);
            }
            if (++index == argc) { throw std::invalid_argument("missing value for " + option); }
            const auto value = utf8(argv[index]);
            if (value.empty() || value.compare(0, 2, "--") == 0) { throw std::invalid_argument("missing value for " + option); }
            if (option == "--device") { device = value; }
            else if (option == "--output") { output = value; }
            else {
                const auto number = rtsp::app::parse_frame_limit(value);
                if (option == "--seconds") {
                    if (number == 0 || number > 3600) { throw std::invalid_argument("seconds must be 1..3600"); }
                    seconds = static_cast<int>(number);
                } else { device_index = static_cast<int>(number); }
            }
        }
        if ((synthetic ? 1 : 0) + (!device.empty() ? 1 : 0) + (device_index >= 0 ? 1 : 0) != 1) {
            throw std::invalid_argument("select exactly one audio input");
        }
        std::unique_ptr<rtsp::AudioFrameInput> input;
        if (synthetic) {
            input = std::make_unique<rtsp::app::SyntheticAudioInput>(seconds * 44100LL + 17);
        } else {
#ifdef RTSP_HAS_MICROPHONE_CAPTURE
            if (device_index >= 0) {
                const auto devices = rtsp::MicrophoneCapture::enumerate_devices();
                if (static_cast<std::size_t>(device_index) >= devices.size()) { throw std::invalid_argument("microphone index out of range"); }
                device = devices[static_cast<std::size_t>(device_index)].name;
            }
            rtsp::MicrophoneConfig capture;
            capture.device_name = device;
            input = rtsp::make_microphone_capture_input(capture);
#else
            throw std::invalid_argument("microphone input requires ENABLE_MICROPHONE_CAPTURE=ON");
#endif
        }
        if (!SetConsoleCtrlHandler(console_control, TRUE)) { throw std::runtime_error("could not install console handler"); }
        std::ofstream file(std::filesystem::u8path(output), std::ios::binary);
        std::ofstream csv(std::filesystem::u8path(output + ".csv"));
        file.exceptions(std::ios::badbit | std::ios::failbit);
        csv.exceptions(std::ios::badbit | std::ios::failbit);
        csv << "kind,pts,samples,rate\n";
        rtsp::AacEncoder encoder;
        encoder.open();
        std::cout << encoder.track().fmtp << '\n';
        const auto publish = [&](const std::vector<rtsp::AacAccessUnit>& frames) {
            for (const auto& frame : frames) {
                write_adts(file, frame);
                csv << "aac," << frame.pts << ',' << frame.duration << ",48000\n";
            }
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        auto last_data = std::chrono::steady_clock::now();
        while (!stopped.load() && (synthetic || std::chrono::steady_clock::now() < deadline)) {
            rtsp::AudioFrame frame;
            const auto result = input->read(frame, std::chrono::milliseconds(50));
            if (result == rtsp::AudioInputResult::Ended) { break; }
            if (result == rtsp::AudioInputResult::Waiting) {
                if (std::chrono::steady_clock::now() - last_data > std::chrono::seconds(5)) {
                    throw std::runtime_error("microphone produced no PCM for 5 seconds");
                }
                continue;
            }
            last_data = std::chrono::steady_clock::now();
            csv << "pcm,";
            if (frame.source_pts) { csv << *frame.source_pts; }
            csv << ',' << frame.nb_samples << ',' << frame.sample_rate << '\n';
            publish(encoder.encode(frame));
        }
        input->close();
        publish(encoder.flush());
        const auto stats = encoder.stats();
        std::ofstream json(std::filesystem::u8path(output + ".json"));
        json.exceptions(std::ios::badbit | std::ios::failbit);
        json << "{\n  \"input_samples\": " << stats.input_samples
             << ",\n  \"resampled_samples\": " << stats.resampled_samples
             << ",\n  \"submitted_samples\": " << stats.submitted_samples
             << ",\n  \"tail_padding\": " << stats.tail_padding
             << ",\n  \"initial_padding\": " << stats.initial_padding
             << ",\n  \"trailing_padding\": " << stats.trailing_padding
             << ",\n  \"skip_samples\": " << stats.skip_samples
             << ",\n  \"discard_padding\": " << stats.discard_padding
             << ",\n  \"packets\": " << stats.packets << "\n}\n";
        std::cout << "input=" << stats.input_samples << " resampled=" << stats.resampled_samples
                  << " submitted=" << stats.submitted_samples << " initial_padding=" << stats.initial_padding
                  << " tail_padding=" << stats.tail_padding << " packets=" << stats.packets << '\n';
        SetConsoleCtrlHandler(console_control, FALSE);
        return 0;
    } catch (const std::exception& error) {
        SetConsoleCtrlHandler(console_control, FALSE);
        std::cerr << "AAC probe error: " << error.what() << '\n';
        return 1;
    }
}
