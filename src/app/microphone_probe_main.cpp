#include "media/microphone_capture.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::atomic<bool> console_stop{false};

BOOL WINAPI on_console_control(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        console_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

class ConsoleScope {
public:
    ConsoleScope() {
        if (!SetConsoleCtrlHandler(on_console_control, TRUE)) {
            throw std::runtime_error("could not register console stop handler");
        }
    }
    ~ConsoleScope() {
        SetConsoleCtrlHandler(on_console_control, FALSE);
    }
};

std::string utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::invalid_argument("invalid Unicode argument");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

int number(const std::string& text, int minimum, int maximum, const std::string& option) {
    if (text.empty()) {
        throw std::invalid_argument("missing number for " + option);
    }
    int value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9' || value > (maximum - (digit - '0')) / 10) {
            throw std::invalid_argument("invalid number for " + option);
        }
        value = value * 10 + (digit - '0');
    }
    if (value < minimum || value > maximum) {
        throw std::invalid_argument("number out of range for " + option);
    }
    return value;
}

void usage() {
    std::cout << "Usage: microphone_probe [options]\n"
              << "  --list                       Enumerate without opening audio devices\n"
              << "  --device NAME                Friendly or alternative name (UTF-8)\n"
              << "  --device-index N             Index from --list; exclusive with --device\n"
              << "  --sample-rate N --channels N  Requested format; 0 uses device defaults\n"
              << "  --buffer-ms N                Requested buffer; 0 uses device default\n"
              << "  --seconds N                  Seconds per run; default 10, 0 unlimited\n"
              << "  --timeout-ms N               Fail after no PCM for N ms; default 5000\n"
              << "  --repeat N                   Close and reopen N times; default 1\n"
              << "  --quiet                      First frame and summary only\n"
              << "  --help                       Show help\n"
              << "Default device: first enumerated input. Ctrl+C / Ctrl+Break stops capture.\n";
}

void print_frame(const rtsp::AudioFrame& frame) {
    std::cout << "[pcm] n=" << frame.sequence << " samples=" << frame.nb_samples
              << " rate=" << frame.sample_rate << " channels=" << frame.channels
              << " fmt=" << frame.sample_format << " source_pts=";
    if (frame.source_pts) {
        std::cout << *frame.source_pts;
    } else {
        std::cout << "missing";
    }
    std::cout << " time_base=" << frame.time_base_num << '/' << frame.time_base_den
              << " arrival_steady_us=" << frame.arrival_steady_us << std::endl;
}

struct Options {
    rtsp::MicrophoneConfig config;
    bool list = false;
    bool quiet = false;
    int device_index = -1;
    int seconds = 10;
    int timeout_ms = 5000;
    int repeat = 1;
};

Options parse_options(int argc, wchar_t* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string option = utf8(argv[index]);
        if (option == "--list") {
            options.list = true;
            continue;
        }
        if (option == "--quiet") {
            options.quiet = true;
            continue;
        }
        if (option != "--device" && option != "--device-index" && option != "--sample-rate"
            && option != "--channels" && option != "--buffer-ms" && option != "--seconds"
            && option != "--timeout-ms" && option != "--repeat") {
            throw std::invalid_argument("unknown option: " + option);
        }
        if (++index == argc) {
            throw std::invalid_argument("missing value for " + option);
        }
        const std::string value = utf8(argv[index]);
        if (option == "--device") {
            if (value.empty()) {
                throw std::invalid_argument("empty microphone device name");
            }
            options.config.device_name = value;
        } else if (option == "--device-index") {
            options.device_index = number(value, 0, 10000, option);
        } else if (option == "--sample-rate") {
            options.config.sample_rate = number(value, 0, 384000, option);
        } else if (option == "--channels") {
            options.config.channels = number(value, 0, 64, option);
        } else if (option == "--buffer-ms") {
            options.config.audio_buffer_ms = number(value, 0, 10000, option);
        } else if (option == "--seconds") {
            options.seconds = number(value, 0, 86400, option);
        } else if (option == "--timeout-ms") {
            options.timeout_ms = number(value, 1, 600000, option);
        } else if (option == "--repeat") {
            options.repeat = number(value, 1, 1000, option);
        }
    }
    if (options.device_index >= 0 && !options.config.device_name.empty()) {
        throw std::invalid_argument("--device and --device-index are mutually exclusive");
    }
    return options;
}

// 仅本线程操作设备；控制台回调设置外部停止标记，read 最多短等 50 ms。
bool capture_once(const Options& options, int run) {
    rtsp::MicrophoneCapture capture;
    const auto start = std::chrono::steady_clock::now();
    try {
        capture.open(options.config, &console_stop);
    } catch (const std::exception&) {
        if (console_stop.load()) {
            return true;
        }
        throw;
    }
    const auto& format = capture.format();
    std::cout << "[microphone] run=" << run << " codec=" << format.codec
              << " rate=" << format.sample_rate << " channels=" << format.channels
              << " fmt=" << format.sample_format << " layout=" << format.channel_layout
              << " time_base=" << format.time_base_num << '/' << format.time_base_den << '\n';
    const auto capture_start = std::chrono::steady_clock::now();
    auto last_frame = capture_start;
    auto next_log = capture_start + std::chrono::seconds(1);
    std::uint64_t frames = 0;
    std::uint64_t samples = 0;
    bool failed = false;
    std::string reason = "duration";
    while (!console_stop.load()) {
        const auto before_read = std::chrono::steady_clock::now();
        if (options.seconds && before_read - capture_start >= std::chrono::seconds(options.seconds)) {
            break;
        }
        const auto no_data_deadline = last_frame + std::chrono::milliseconds(options.timeout_ms);
        if (before_read >= no_data_deadline) {
            std::cerr << "Microphone error: no PCM within " << options.timeout_ms << " ms\n";
            reason = "no_pcm";
            failed = true;
            break;
        }
        auto deadline = std::min(before_read + std::chrono::milliseconds(50), no_data_deadline);
        if (options.seconds) {
            deadline = std::min(deadline, capture_start + std::chrono::seconds(options.seconds));
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - before_read);
        rtsp::AudioFrame frame;
        const auto result = capture.read(frame, wait);
        if (result == rtsp::MicrophoneReadResult::Stopped) {
            reason = "stopped";
            break;
        }
        if (result == rtsp::MicrophoneReadResult::Failed) {
            std::cerr << "Microphone error: " << capture.last_error() << '\n';
            reason = "failed";
            failed = true;
            break;
        }
        if (result == rtsp::MicrophoneReadResult::Timeout) {
            continue;
        }
        last_frame = std::chrono::steady_clock::now();
        ++frames;
        samples += static_cast<std::uint64_t>(frame.nb_samples);
        if (frames == 1 || (!options.quiet && last_frame >= next_log)) {
            print_frame(frame);
            std::cout << "[stats] frames=" << frames << " samples=" << samples << '\n';
            next_log = last_frame + std::chrono::seconds(1);
        }
    }
    if (console_stop.load()) {
        reason = "stopped";
    }
    if (!failed && frames == 0 && reason != "stopped") {
        std::cerr << "Microphone error: capture ended without PCM\n";
        failed = true;
        reason = "no_pcm";
    }
    const int sample_rate = format.sample_rate;
    capture.request_stop();
    const auto close_start = std::chrono::steady_clock::now();
    capture.close();
    const auto closed = std::chrono::steady_clock::now();
    std::cout << std::fixed << std::setprecision(3)
              << "[microphone] released run=" << run << " reason=" << reason
              << " frames=" << frames << " samples=" << samples
              << " pcm_s=" << (sample_rate ? static_cast<double>(samples) / sample_rate : 0.0)
              << " elapsed_s=" << std::chrono::duration<double>(closed - start).count()
              << " close_ms=" << std::chrono::duration<double, std::milli>(closed - close_start).count() << '\n';
    return !failed;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        for (int index = 1; index < argc; ++index) {
            if (utf8(argv[index]) == "--help") {
                usage();
                return 0;
            }
        }
        auto options = parse_options(argc, argv);
        if (options.list || options.config.device_name.empty()) {
            const auto devices = rtsp::MicrophoneCapture::enumerate_devices();
            if (options.list) {
                std::cout << "FFmpeg " << rtsp::MicrophoneCapture::ffmpeg_version() << '\n';
                for (std::size_t index = 0; index < devices.size(); ++index) {
                    std::cout << '[' << index << "] " << devices[index].description
                              << "\n  " << devices[index].name << '\n';
                }
                if (devices.empty()) {
                    std::cout << "No microphone devices found.\n";
                }
                return 0;
            }
            if (devices.empty()) {
                throw std::runtime_error("no microphone devices found; use --list");
            }
            const auto index = static_cast<std::size_t>(options.device_index < 0 ? 0 : options.device_index);
            if (index >= devices.size()) {
                throw std::invalid_argument("microphone index out of range; use --list");
            }
            options.config.device_name = devices[index].name;
        }
        ConsoleScope console;
        for (int run = 1; run <= options.repeat && !console_stop.load(); ++run) {
            if (!capture_once(options, run)) {
                return 1;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Microphone error: " << error.what() << '\n';
        return 1;
    }
}
