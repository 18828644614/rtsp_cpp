#include "media/h264_encoder.h"
#include "media/h264_file_reader.h"
#include "media/h264_file_writer.h"
#include "sdp/session_description.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace {

void check(bool valid, const char* message) {
    if (!valid) { throw std::runtime_error(message); }
}

template<class Exception = std::invalid_argument, class Action>
void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const Exception&) { rejected = true; }
    check(rejected, "expected operation to reject invalid input/state");
}

int grey(int x, int y, int index) { return 40 + index * 2 + (x / 16 + y / 16) % 15; }

rtsp::CameraFrame make_frame(const rtsp::H264EncoderConfig& config, int index) {
    auto* raw = av_frame_alloc();
    if (!raw) { throw std::bad_alloc(); }
    std::shared_ptr<AVFrame> owned(raw, [](AVFrame* frame) { av_frame_free(&frame); });
    raw->width = config.width;
    raw->height = config.height;
    raw->format = config.pixel_format == "yuyv422" ? AV_PIX_FMT_YUYV422 : AV_PIX_FMT_BGR24;
    check(av_frame_get_buffer(raw, 64) >= 0, "allocate test frame");
    const int bytes_per_pixel = raw->format == AV_PIX_FMT_YUYV422 ? 2 : 3;
    check(raw->linesize[0] > config.width * bytes_per_pixel, "test needs padded source rows");
    for (int y = 0; y < config.height; ++y) {
        auto* row = raw->data[0] + y * raw->linesize[0];
        for (int x = 0; x < config.width; ++x) {
            const auto value = static_cast<std::uint8_t>(grey(x, y, index));
            if (raw->format == AV_PIX_FMT_YUYV422) {
                row[x * 2] = value;
                row[x * 2 + 1] = static_cast<std::uint8_t>(x % 2 == 0 ? 96 : 160);
            } else {
                row[x * 3] = static_cast<std::uint8_t>(value + 20);
                row[x * 3 + 1] = value;
                row[x * 3 + 2] = static_cast<std::uint8_t>(value + 40);
            }
        }
    }
    rtsp::CameraFrame frame;
    frame.buffer = std::move(owned);
    frame.width = raw->width;
    frame.height = raw->height;
    frame.pixel_format = config.pixel_format;
    frame.linesize[0] = raw->linesize[0];
    frame.sequence = static_cast<std::uint64_t>(index);
    frame.source_pts = index;
    frame.time_base_num = config.fps_den;
    frame.time_base_den = config.fps_num;
    frame.source_duration = 1;
    return frame;
}

rtsp::H264EncoderConfig small_config() {
    rtsp::H264EncoderConfig config;
    config.width = 318;
    config.height = 178;
    config.gop = 8;
    config.bitrate = 600000;
    return config;
}

class Decoder {
public:
    explicit Decoder(rtsp::H264EncoderConfig config) : config_(std::move(config)) {
        const auto* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        check(codec != nullptr, "H264 decoder unavailable");
        context_ = avcodec_alloc_context3(codec);
        check(context_ != nullptr, "allocate decoder context");
        context_->pkt_timebase = {1, 90000};
        context_->thread_count = 1;
        try {
            check(avcodec_open2(context_, codec, nullptr) >= 0, "open H264 decoder");
            packet_ = av_packet_alloc();
            frame_ = av_frame_alloc();
            check(packet_ && frame_, "allocate decoder buffers");
        } catch (...) { release(); throw; }
    }
    ~Decoder() { release(); }

    void submit(const rtsp::H264AccessUnit& unit, int source_index) {
        std::string bytes;
        for (const auto& nalu : unit.nalus) { bytes.append("\0\0\0\1", 4); bytes += nalu; }
        check(av_new_packet(packet_, static_cast<int>(bytes.size())) >= 0, "allocate decoder packet");
        std::copy(bytes.begin(), bytes.end(), packet_->data);
        packet_->pts = packet_->dts = static_cast<std::int64_t>(unit.pts);
        packet_->duration = unit.duration;
        expected_.emplace(packet_->pts, source_index);
        check(avcodec_send_packet(context_, packet_) >= 0, "send encoded AU to decoder");
        av_packet_unref(packet_);
        receive();
    }

    void finish(std::size_t expected_count) {
        check(avcodec_send_packet(context_, nullptr) >= 0, "flush H264 decoder");
        check(receive(), "decoder did not reach EOF");
        check(count_ == expected_count && expected_.empty(), "decoder lost or duplicated pictures");
        std::cout << "decoded=" << count_ << " maximum_luma_mae=" << max_mae_ << '\n';
    }

private:
    void release() {
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&context_);
    }
    bool receive() {
        for (;;) {
            const int result = avcodec_receive_frame(context_, frame_);
            if (result == AVERROR(EAGAIN)) { return false; }
            if (result == AVERROR_EOF) { return true; }
            check(result >= 0, "decode encoded picture failed");
            check(frame_->width == config_.width && frame_->height == config_.height,
                "decoded dimensions differ from input");
            check(frame_->format == AV_PIX_FMT_YUV420P, "decoder output is not YUV420P");
            check(frame_->color_range == AVCOL_RANGE_MPEG && frame_->colorspace == AVCOL_SPC_SMPTE170M,
                "H264 bitstream did not preserve the declared output color interpretation");
            const auto found = expected_.find(frame_->pts);
            check(found != expected_.end(), "decoded PTS does not match an encoded AU");
            double error = 0;
            for (int y = 0; y < frame_->height; ++y) {
                const auto* row = frame_->data[0] + y * frame_->linesize[0];
                for (int x = 0; x < frame_->width; ++x) {
                    const auto value = grey(x, y, found->second);
                    const auto expected = config_.pixel_format == "yuyv422" ? value :
                        16 + static_cast<int>(std::lround(219.0 * value / 255.0 + 0.257 * 40 + 0.098 * 20));
                    error += std::abs(static_cast<int>(row[x]) - expected);
                }
            }
            const double mae = error / (frame_->width * frame_->height);
            check(mae < 6.0, "decoded picture differs too much from the corresponding source image");
            for (int plane = 1; plane <= 2; ++plane) {
                const int expected = config_.pixel_format == "yuyv422" ? (plane == 1 ? 96 : 160) :
                    (plane == 1 ? 131 : 144);
                double chroma_error = 0;
                for (int y = 0; y < frame_->height / 2; ++y) {
                    const auto* row = frame_->data[plane] + y * frame_->linesize[plane];
                    for (int x = 0; x < frame_->width / 2; ++x) {
                        chroma_error += std::abs(static_cast<int>(row[x]) - expected);
                    }
                }
                check(chroma_error / ((frame_->width / 2) * (frame_->height / 2)) < 6.0,
                    "decoded chroma indicates swapped channels or incorrect YUV conversion");
            }
            max_mae_ = std::max(max_mae_, mae);
            expected_.erase(found);
            ++count_;
            av_frame_unref(frame_);
        }
    }
    rtsp::H264EncoderConfig config_;
    AVCodecContext* context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    std::map<std::int64_t, int> expected_;
    std::size_t count_ = 0;
    double max_mae_ = 0;
};

void append(std::vector<rtsp::H264AccessUnit>& output, std::vector<rtsp::H264AccessUnit> units) {
    for (auto& unit : units) { output.push_back(std::move(unit)); }
}

void validation_tests() {
    rtsp::H264Encoder encoder;
    rejects<std::logic_error>([&] { encoder.encode({}); });
    rejects<std::logic_error>([&] { encoder.flush(); });
    auto config = small_config();
    auto bad = config;
    bad.width = 319;
    rejects([&] { encoder.open(bad); });
    bad = config; bad.height = 0;
    rejects([&] { encoder.open(bad); });
    bad = config; bad.fps_den = 0;
    rejects([&] { encoder.open(bad); });
    bad = config; bad.fps_num = 241;
    rejects([&] { encoder.open(bad); });
    bad = config; bad.bitrate = 0;
    rejects([&] { encoder.open(bad); });
    bad = config; bad.gop = 0;
    rejects([&] { encoder.open(bad); });
    bad = config; bad.pixel_format = "mjpeg";
    rejects([&] { encoder.open(bad); });
    for (int run = 0; run < 3; ++run) {
        encoder.open(config);
        check(encoder.sps().empty() && encoder.pps().empty(), "reopen retained stale parameter sets");
        rejects([&] { encoder.encode({}); });
        auto input = make_frame(config, 0);
        auto wrong = input;
        wrong.width += 2;
        rejects([&] { encoder.encode(wrong); });
        wrong = input; wrong.pixel_format = "bgr24";
        rejects([&] { encoder.encode(wrong); });
        wrong = input; wrong.time_base_den = 0;
        rejects([&] { encoder.encode(wrong); });
        wrong = input; wrong.source_duration = -1;
        rejects([&] { encoder.encode(wrong); });
        wrong = input; wrong.source_duration = std::numeric_limits<std::int64_t>::max();
        rejects([&] { encoder.encode(wrong); });
        std::vector<rtsp::H264AccessUnit> units;
        append(units, encoder.encode(input));
        const auto saved = units;
        rejects([&] { encoder.encode(input); }); // 重复 PTS，不破坏编码器，后续合法帧仍能编码。
        auto next = make_frame(config, 1);
        wrong = next; wrong.time_base_num *= 2;
        rejects([&] { encoder.encode(wrong); });
        append(units, encoder.encode(next));
        append(units, encoder.flush());
        check(units.size() == 2 && !saved.empty() && saved[0].nalus == units[0].nalus,
            "output memory did not survive later encoding/flush");
        check(encoder.flush().empty(), "repeated flush must be empty");
        rejects<std::logic_error>([&] { encoder.encode(next); });
        encoder.close(); encoder.close();
        check(encoder.sps().empty() && encoder.timestamp_fallbacks() == 0, "close did not clear encoder state");
        Decoder decoder(config);
        decoder.submit(units[0], 0);
        decoder.submit(units[1], 1);
        decoder.finish(2);
    }
    encoder.open(config);
    check(encoder.flush().empty(), "empty stream flush produced a picture");
    auto short_stride = make_frame(config, 0);
    encoder.open(config);
    const_cast<AVFrame*>(short_stride.buffer.get())->linesize[0] = 1;
    rejects([&] { encoder.encode(short_stride); });
    auto wrong_range = make_frame(config, 0);
    const_cast<AVFrame*>(wrong_range.buffer.get())->color_range = AVCOL_RANGE_JPEG;
    rejects([&] { encoder.encode(wrong_range); });
}

void stream_test(const std::string& format, int fps_num, int fps_den) {
    auto config = small_config();
    config.pixel_format = format;
    config.fps_num = fps_num;
    config.fps_den = fps_den;
    rtsp::H264Encoder encoder;
    encoder.open(config);
    std::vector<rtsp::H264AccessUnit> units;
    constexpr int frames = 32;
    for (int index = 0; index < frames; ++index) {
        const auto input = make_frame(config, index);
        const auto first_byte = input.buffer->data[0][0];
        append(units, encoder.encode(input));
        check(input.buffer->data[0][0] == first_byte && input.buffer->pts == AV_NOPTS_VALUE,
            "encoder modified the caller's frame");
    }
    append(units, encoder.flush());
    check(units.size() == frames, "encode/flush lost or duplicated access units");
    check(encoder.timestamp_mode() == rtsp::EncoderTimestampMode::Source && encoder.timestamp_fallbacks() == 0,
        "source timing unexpectedly used fallback");
    check(!rtsp::make_h264_fmtp(encoder.sps(), encoder.pps()).empty(), "SPS/PPS cannot generate SDP fmtp");
    rtsp::H264FileWriter writer;
    const std::string path = "h264-encoder-tests-" + format + "-" + std::to_string(fps_num) + ".h264";
    writer.open(path);
    Decoder decoder(config);
    bool multiple_slices = false;
    for (int index = 0; index < frames; ++index) {
        const auto& unit = units[static_cast<std::size_t>(index)];
        check(unit.pts == static_cast<std::uint64_t>(av_rescale_q(index, {fps_den, fps_num}, {1, 90000})),
            "AU PTS used a fixed 25 fps clock or accumulated rounding error");
        check(unit.duration == static_cast<std::uint32_t>(av_rescale_q(1, {fps_den, fps_num}, {1, 90000})),
            "AU duration does not match source duration");
        int aud = 0; int slices = 0; bool sps = false; bool pps = false; bool idr = false;
        for (const auto& nalu : unit.nalus) {
            const auto type = static_cast<unsigned char>(nalu[0]) & 0x1f;
            aud += type == 9;
            slices += type == 1 || type == 5;
            idr = idr || type == 5; sps = sps || type == 7; pps = pps || type == 8;
            writer.write(nalu);
        }
        check(aud == 1 && (static_cast<unsigned char>(unit.nalus.front()[0]) & 0x1f) == 9 && slices > 0,
            "AU boundary or original NALU representation is invalid");
        multiple_slices = multiple_slices || slices > 1;
        check(unit.is_keyframe == idr && idr == (index % config.gop == 0), "IDR flag/GOP does not match bitstream");
        check(!idr || (sps && pps), "IDR cannot be decoded without earlier parameter sets");
        decoder.submit(unit, index);
    }
    writer.close();
    decoder.finish(frames);
    check(multiple_slices, "test did not exercise an access unit with multiple slices");
    // 中途从第二个 IDR 启动一个全新解码器，验证重复参数集确实可用。
    Decoder late_decoder(config);
    for (int index = config.gop; index < frames; ++index) { late_decoder.submit(units[index], index); }
    late_decoder.finish(frames - config.gop);
    rtsp::H264FileReader reader;
    reader.open(path);
    check(reader.frame_count() == frames, "Annex-B file is incompatible with the existing AUD file reader");
    for (const auto& unit : units) {
        rtsp::H264AccessUnit read;
        check(reader.read(read) && read.nalus == unit.nalus && read.is_keyframe == unit.is_keyframe,
            "file round-trip changed AU boundaries or NALU bytes");
    }
    rtsp::H264AccessUnit end;
    check(!reader.read(end), "file reader returned extra pictures");
}

void timing_tests() {
    auto config = small_config();
    rtsp::H264Encoder encoder;
    encoder.open(config);
    std::vector<rtsp::H264AccessUnit> units;
    for (int index = 0; index < 4; ++index) {
        auto frame = make_frame(config, index);
        frame.source_pts = -1000 + index * 333333LL + (index >= 2 ? 333333 : 0);
        frame.time_base_num = 1; frame.time_base_den = 10000000;
        frame.source_duration = 333333;
        append(units, encoder.encode(frame));
    }
    append(units, encoder.flush());
    check(units.size() == 4 && units[0].pts == 0 && units[1].pts == 3000 && units[2].pts == 9000,
        "encoder did not preserve source timing gaps/negative origin");
    encoder.open(config); units.clear();
    for (int index = 0; index < 4; ++index) {
        auto frame = make_frame(config, index);
        if (index == 1) { frame.source_pts.reset(); }
        append(units, encoder.encode(frame));
    }
    append(units, encoder.flush());
    check(encoder.timestamp_fallbacks() == 1 && units[1].pts == 3000 && units[2].pts == 6000,
        "intermittent missing timestamp fallback is incorrect");
    config.fps_num = 24000; config.fps_den = 1001;
    encoder.open(config); units.clear();
    for (int index = 0; index < 10; ++index) {
        auto frame = make_frame(config, index);
        if (index == 0 || index % 2 == 0) { frame.source_pts.reset(); }
        frame.source_duration = 0;
        append(units, encoder.encode(frame));
    }
    append(units, encoder.flush());
    check(encoder.timestamp_mode() == rtsp::EncoderTimestampMode::FrameRate && encoder.timestamp_fallbacks() == 10,
        "nominal timing mode must remain fixed after a missing first PTS");
    for (int index = 0; index < 10; ++index) {
        const auto begin = av_rescale_q(index, {1001, 24000}, {1, 90000});
        const auto end = av_rescale_q(index + 1, {1001, 24000}, {1, 90000});
        check(units[index].pts == static_cast<std::uint64_t>(begin) && units[index].duration == end - begin,
            "nominal fractional frame rate accumulated timestamp error");
    }
    encoder.open(config);
    auto frame = make_frame(config, 0);
    frame.source_pts = std::numeric_limits<std::int64_t>::min();
    encoder.encode(frame);
    frame.source_pts = std::numeric_limits<std::int64_t>::max();
    rejects([&] { encoder.encode(frame); });
    encoder.flush();
}

}

int main() {
    try {
        av_log_set_level(AV_LOG_ERROR);
        validation_tests();
        stream_test("yuyv422", 30, 1);
        stream_test("bgr24", 25, 1);
        stream_test("yuyv422", 30000, 1001);
        timing_tests();
        std::cout << "h264_encoder_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "h264_encoder_tests failed: " << error.what() << '\n';
        return 1;
    }
}
