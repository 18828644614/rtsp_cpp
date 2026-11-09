#include "app/server_options.h"
#include "media/media_session.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void check(bool ok) { if (!ok) { throw std::runtime_error("server options mismatch"); } }
void rejects(const std::vector<std::string>& args) {
    try { rtsp::app::parse_server_options(args); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("accepted invalid server options");
}

void unicode_file_paths() {
    // 入口将宽字符参数转换成 UTF-8，文件读取也必须使用 UTF-8 路径。
    const std::string video = "server-中文.h264";
    const std::string audio = "server-中文.aac";
    const auto video_path = std::filesystem::u8path(video);
    const auto audio_path = std::filesystem::u8path(audio);
    try {
        const std::string h264("\0\0\0\1\x09\xf0\0\0\0\1\x67\x64\0\x1f\0\0\0\1\x68\xee\0\0\0\1\x65\xab", 26);
        const std::string aac("\xff\xf1\x4c\x80\x01\x5f\xfc\x11\x22\x33", 10);
        std::ofstream(video_path, std::ios::binary).write(h264.data(), h264.size());
        std::ofstream(audio_path, std::ios::binary).write(aac.data(), aac.size());
        rtsp::MediaSession media(video, audio);
        check(media.description().tracks.size() == 2);
    } catch (...) {
        std::filesystem::remove(video_path); std::filesystem::remove(audio_path); throw;
    }
    std::filesystem::remove(video_path); std::filesystem::remove(audio_path);
}
}

int main() {
    try {
        auto options = rtsp::app::parse_server_options({});
        check(!options.camera() && options.port == 8554 && options.video_path == "samples/sample.h264");
        options = rtsp::app::parse_server_options({"1234", "0.0.0.0", "-", "a.aac"});
        check(options.port == 1234 && options.address == "0.0.0.0" && options.video_path.empty()
              && options.audio_path == "a.aac");
        options = rtsp::app::parse_server_options({"8554", "127.0.0.1", "中文路径.h264"});
        check(options.video_path == "中文路径.h264");
        check(rtsp::app::parse_server_options({"--help"}).help);
        check(rtsp::app::parse_server_options({"--list-cameras"}).list_cameras);
        options = rtsp::app::parse_server_options({"--camera", "0", "--fps", "60", "--bind", "0.0.0.0"});
        check(options.camera() && options.camera_index == 0 && options.gop == 60 && options.video_path.empty());
        options = rtsp::app::parse_server_options({"--device", "USB 摄像头", "--width", "640", "--height", "480",
            "--fps", "25", "--bitrate", "1000000", "--gop", "50", "--pixel-format", "bgr24", "--port", "9000"});
        check(options.device_name == "USB 摄像头" && options.width == 640 && options.height == 480
              && options.fps == 25 && options.bitrate == 1000000 && options.gop == 50
              && options.pixel_format == "bgr24" && options.port == 9000);
        for (const auto& args : std::vector<std::vector<std::string>>{
            {"--camera"}, {"--camera", "--fps", "30"}, {"--camera", "-1"}, {"--camera", "1x"},
            {"--device", ""}, {"--camera", "0", "--device", "x"}, {"--fps", "30"},
            {"--camera", "0", "--width", "641"}, {"--camera", "0", "--height", "0"},
            {"--camera", "0", "--fps", "241"}, {"--camera", "0", "--bitrate", "2147483648"},
            {"--camera", "0", "--gop", "0"}, {"--camera", "0", "--pixel-format", "mjpeg"},
            {"--camera", "0", "--port", "0"}, {"--camera", "0", "--port", "999999999999999999"},
            {"--camera", "0", "--camera", "1"}, {"--camera", "0", "sample.h264"},
            {"8554", "127.0.0.1", "--camera", "0"}, {"--list-cameras", "--camera", "0"},
            {"--not-an-option"}, {"1", "2", "3", "4", "5"}}) { rejects(args); }
        unicode_file_paths();
        std::cout << "PASS: file compatibility, camera selection, Unicode and invalid options\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
