#include "media/h264_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace rtsp {
namespace {

constexpr AVRational media_time_base{1, 90000};

void require_success(int result, const char* operation) {
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, error, sizeof(error));
        throw std::runtime_error(std::string(operation) + ": " + error);
    }
}

void validate(const H264EncoderConfig& config) {
    if (config.width <= 0 || config.width > 16384 || config.width % 2 != 0 ||
        config.height <= 0 || config.height > 16384 || config.height % 2 != 0 ||
        config.fps_num <= 0 || config.fps_den <= 0 ||
        av_q2d({config.fps_num, config.fps_den}) > 240.0 ||
        av_q2d({config.fps_num, config.fps_den}) < 1.0 ||
        config.bitrate <= 0 || config.gop <= 0 || config.gop > 10000) {
        throw std::invalid_argument("invalid encoder dimensions (must be even), frame rate, bitrate or GOP");
    }
    if (config.pixel_format != "yuyv422" && config.pixel_format != "bgr24") {
        throw std::invalid_argument("encoder input must be yuyv422 or bgr24");
    }
}

std::size_t start_code(const std::uint8_t* data, std::size_t size,
                       std::size_t begin, std::size_t& prefix) {
    for (auto offset = begin; offset + 2 < size; ++offset) {
        if (data[offset] != 0 || data[offset + 1] != 0) { continue; }
        if (data[offset + 2] == 1) { prefix = 3; return offset; }
        if (offset + 3 < size && data[offset + 2] == 0 && data[offset + 3] == 1) {
            prefix = 4;
            return offset;
        }
    }
    prefix = 0;
    return size;
}

// 首版限定 libx264 的 Annex-B 输出：一个编码包必须恰好对应一个带 AUD 的图像 AU。
H264AccessUnit parse_packet(const AVPacket& packet) {
    if (!packet.data || packet.size <= 0) { throw std::runtime_error("empty H264 encoder packet"); }
    const auto size = static_cast<std::size_t>(packet.size);
    std::size_t prefix = 0;
    auto position = start_code(packet.data, size, 0, prefix);
    if (position == size || std::any_of(packet.data, packet.data + position,
        [](std::uint8_t value) { return value != 0; })) {
        throw std::runtime_error("encoder did not produce Annex-B H264");
    }
    H264AccessUnit unit;
    bool has_picture = false;
    while (position < size) {
        const auto begin = position + prefix;
        std::size_t next_prefix = 0;
        const auto next = start_code(packet.data, size, begin, next_prefix);
        auto end = next;
        while (end > begin && packet.data[end - 1] == 0) { --end; }
        if (end == begin) { throw std::runtime_error("empty encoder NALU"); }
        const unsigned int header = packet.data[begin];
        const auto type = header & 0x1f;
        if ((header & 0x80) || type == 0 || type > 23) {
            throw std::runtime_error("invalid encoder NALU header");
        }
        if ((unit.nalus.empty() && type != 9) || (type == 9 && !unit.nalus.empty())) {
            throw std::runtime_error("encoder packet must contain exactly one AUD at its beginning");
        }
        has_picture = has_picture || (type >= 1 && type <= 5);
        unit.is_keyframe = unit.is_keyframe || type == 5;
        unit.nalus.emplace_back(reinterpret_cast<const char*>(packet.data + begin), end - begin);
        position = next;
        prefix = next_prefix;
    }
    if (!has_picture) { throw std::runtime_error("encoder access unit has no picture"); }
    return unit;
}

}

struct H264Encoder::Impl {
    struct Timing {
        std::int64_t pts;
        std::uint32_t duration;
    };

    AVCodecContext* codec = nullptr;
    SwsContext* converter = nullptr;
    AVFrame* converted = nullptr;
    AVPacket* packet = nullptr;
    H264EncoderConfig config;
    AVPixelFormat input_format = AV_PIX_FMT_NONE;
    std::map<std::int64_t, std::uint32_t> pending;
    std::string sps;
    std::string pps;
    EncoderTimestampMode mode = EncoderTimestampMode::Unset;
    std::optional<std::int64_t> source_origin;
    AVRational source_time_base{0, 1};
    std::optional<std::int64_t> last_pts;
    std::int64_t submitted = 0;
    std::uint64_t fallbacks = 0;
    bool flushed = false;
    bool failed = false;

    ~Impl() { release(); }

    void release() noexcept {
        av_packet_free(&packet);
        av_frame_free(&converted);
        sws_freeContext(converter);
        converter = nullptr;
        avcodec_free_context(&codec);
        pending.clear();
        sps.clear();
        pps.clear();
        mode = EncoderTimestampMode::Unset;
        source_origin.reset();
        source_time_base = {0, 1};
        last_pts.reset();
        submitted = 0;
        fallbacks = 0;
        flushed = false;
        failed = false;
    }

    void require_open() const {
        if (!codec) { throw std::logic_error("H264 encoder is not open"); }
        if (failed) { throw std::logic_error("H264 encoder failed; reopen it before use"); }
    }

    Timing timing(const CameraFrame& frame) const {
        const bool has_time_base = frame.time_base_num > 0 && frame.time_base_den > 0;
        if ((frame.source_pts || frame.source_duration != 0) && !has_time_base) {
            throw std::invalid_argument("source timestamp/duration needs a positive time base");
        }
        if (frame.source_duration < 0 || submitted == std::numeric_limits<std::int64_t>::max()) {
            throw std::invalid_argument("invalid source duration or exhausted frame counter");
        }
        const AVRational interval{config.fps_den, config.fps_num};
        const auto nominal_pts = av_rescale_q(submitted, interval, media_time_base);
        const auto next_pts = av_rescale_q(submitted + 1, interval, media_time_base);
        if (nominal_pts < 0 || next_pts <= nominal_pts) {
            throw std::invalid_argument("encoder timeline overflow");
        }
        const auto step = next_pts - nominal_pts;
        std::int64_t pts = nominal_pts;
        const bool use_source = mode == EncoderTimestampMode::Source ||
            (mode == EncoderTimestampMode::Unset && frame.source_pts.has_value());
        if (use_source && frame.source_pts) {
            if (source_origin && (frame.time_base_num != source_time_base.num ||
                frame.time_base_den != source_time_base.den)) {
                throw std::invalid_argument("camera time base changed; reopen encoder");
            }
            const auto origin = source_origin.value_or(*frame.source_pts);
            if (*frame.source_pts < origin || (origin < 0 &&
                *frame.source_pts > std::numeric_limits<std::int64_t>::max() + origin)) {
                throw std::invalid_argument("source timestamp moved backwards or overflowed");
            }
            pts = av_rescale_q(*frame.source_pts - origin,
                {frame.time_base_num, frame.time_base_den}, media_time_base);
        } else if (use_source && last_pts) {
            if (*last_pts > std::numeric_limits<std::int64_t>::max() - step) {
                throw std::invalid_argument("encoder timeline overflow");
            }
            pts = *last_pts + step;
        }
        if (pts < 0 || (last_pts && pts <= *last_pts)) {
            throw std::invalid_argument("encoder PTS must strictly increase after 90 kHz conversion");
        }
        const auto duration = frame.source_duration > 0
            ? av_rescale_q(frame.source_duration,
                {frame.time_base_num, frame.time_base_den}, media_time_base) : step;
        if (duration <= 0 || duration > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("encoder duration is outside the positive 90 kHz range");
        }
        return {pts, static_cast<std::uint32_t>(duration)};
    }

    void parameter_sets(H264AccessUnit& unit) {
        bool has_sps = false;
        bool has_pps = false;
        for (const auto& nalu : unit.nalus) {
            const auto type = static_cast<unsigned char>(nalu[0]) & 0x1f;
            if (type == 7) { sps = nalu; has_sps = true; }
            if (type == 8) { pps = nalu; has_pps = true; }
        }
        if (sps.size() < 4 || pps.empty()) {
            throw std::runtime_error("encoder produced a picture before valid SPS/PPS");
        }
        if (unit.is_keyframe && (!has_sps || !has_pps)) {
            std::vector<std::string> nalus{unit.nalus.front(), sps, pps};
            for (std::size_t index = 1; index < unit.nalus.size(); ++index) {
                const auto type = static_cast<unsigned char>(unit.nalus[index][0]) & 0x1f;
                if (type != 7 && type != 8) { nalus.push_back(std::move(unit.nalus[index])); }
            }
            unit.nalus = std::move(nalus);
        }
    }

    // 返回 true 表示已排空到 EOF，false 表示需要更多输入。
    bool receive(std::vector<H264AccessUnit>& output) {
        for (;;) {
            const int result = avcodec_receive_packet(codec, packet);
            if (result == AVERROR(EAGAIN)) { return false; }
            if (result == AVERROR_EOF) { return true; }
            require_success(result, "receive H264 packet");
            try {
                const auto found = pending.find(packet->pts);
                if (packet->pts == AV_NOPTS_VALUE || found == pending.end() ||
                    (packet->dts != AV_NOPTS_VALUE && packet->dts != packet->pts)) {
                    throw std::runtime_error("encoder packet timestamp does not match a submitted frame");
                }
                auto unit = parse_packet(*packet);
                unit.pts = static_cast<std::uint64_t>(found->first);
                unit.duration = found->second;
                parameter_sets(unit);
                output.push_back(std::move(unit));
                pending.erase(found);
                av_packet_unref(packet);
            } catch (...) {
                av_packet_unref(packet);
                throw;
            }
        }
    }
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;

void H264Encoder::open(const H264EncoderConfig& config) {
    close();
    validate(config);
    // 根据名称查找编码器。
    // AVCodec 描述的是 这是什么编码器、有什么能力
    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) { throw std::runtime_error("FFmpeg SDK requires the libx264 H264 encoder"); }

    // 创建本次编码任务的内部状态
    auto state = std::make_unique<Impl>();
    state->config = config;
    // 把字符串转换为 FFmpeg 枚举
    // "yuyv422" → AV_PIX_FMT_YUYV422
    // "bgr24"   → AV_PIX_FMT_BGR24
    state->input_format = av_get_pix_fmt(config.pixel_format.c_str());
    // 分配一个 AVCodecContext，并初始化与该编码器相关的默认配置
    // AVCodec
    //     编码器的描述
    // AVCodecContext
    //     当前这一次编码任务的配置和运行状态
    state->codec = avcodec_alloc_context3(encoder);
    if (!state->codec) { throw std::bad_alloc(); }
    auto* codec = state->codec;

    codec->width = config.width;
    codec->height = config.height;
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->time_base = media_time_base;
    codec->framerate = {config.fps_num, config.fps_den};
    codec->bit_rate = config.bitrate;
    codec->gop_size = config.gop;
    codec->max_b_frames = 0;
    codec->thread_count = 2;
    // 首版明确使用 BT.601：YUYV 输入为 limited range，BGR 输入为 full range。
    codec->color_range = AVCOL_RANGE_MPEG;
    codec->colorspace = AVCOL_SPC_SMPTE170M;
    codec->color_primaries = AVCOL_PRI_SMPTE170M;
    codec->color_trc = AVCOL_TRC_SMPTE170M;
    // zerolatency 使用 slice 线程，输出可含多个 slice，但仍是一个完整图像包。
    // 通过字典设置 libx264 专用选项
    AVDictionary* options = nullptr;
    try {
        require_success(av_dict_set(&options, "preset", "veryfast", 0), "set x264 preset");
        require_success(av_dict_set(&options, "tune", "zerolatency", 0), "set x264 tune");
        require_success(av_dict_set(&options, "profile", "baseline", 0), "set x264 profile");
        require_success(av_dict_set(&options, "x264-params",
            "annexb=1:repeat-headers=1:aud=1:scenecut=0:open-gop=0", 0), "set x264 parameters");

        // 打开编码器
        // | 参数             | 作用                       |
        // | `codec`         | 已填写通用参数的编码上下文   |
        // | `encoder`       | 要使用的 libx264 编码器描述  |
        // | `&options`      | 额外的通用或编码器专用选项   |
        require_success(avcodec_open2(codec, encoder, &options), "open libx264 encoder");

        // avcodec_open2() 会把未识别的字典选项留在字典中。
        // 如果有剩余，通常意味着传入的选项名称不被支持，不能假定配置已经全部生效。这里检查的是顶层字典选项是否被接收，不等于逐项验证所有 x264 内部行为。
        if (av_dict_count(options)) { throw std::runtime_error("unused H264 encoder options"); }
        av_dict_free(&options);
    } catch (...) {
        av_dict_free(&options);
        throw;
    }

    // 创建像素转换上下文
    state->converter = sws_getContext(config.width, config.height, state->input_format,
        config.width, config.height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    // 接着分配原始帧对象和编码包对象
    // AVFrame：
    //     描述一幅未压缩图像，
    //     包括尺寸、像素格式、平面地址、行跨度、PTS。
    // AVPacket：
    //     描述一份压缩后的编码数据，
    //     包括数据地址、大小、PTS、DTS。
    state->converted = av_frame_alloc();
    state->packet = av_packet_alloc();
    if (!state->converter || !state->converted || !state->packet) { throw std::bad_alloc(); }

    // 明确设置转换器使用的颜色规则:
    // 根据配置准备好编码器、像素转换器和缓冲区，让后面的 encode() 可以开始接收原始图像
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU601);
    require_success(sws_setColorspaceDetails(state->converter,
        coefficients, state->input_format == AV_PIX_FMT_BGR24 ? 1 : 0,
        coefficients, 0, 0, 1 << 16, 1 << 16), "set encoder BT.601 conversion");
    state->converted->format = AV_PIX_FMT_YUV420P;
    state->converted->width = config.width;
    state->converted->height = config.height;
    state->converted->color_range = codec->color_range;
    state->converted->colorspace = codec->colorspace;
    state->converted->color_primaries = codec->color_primaries;
    state->converted->color_trc = codec->color_trc;
    require_success(av_frame_get_buffer(state->converted, 32), "allocate encoder conversion frame");
    impl_ = std::move(state);
}

std::vector<H264AccessUnit> H264Encoder::encode(const CameraFrame& frame) {
    // 检查编码器是否处于可接收输入的状态
    impl_->require_open();
    if (impl_->flushed) { throw std::logic_error("cannot encode after flush; reopen encoder"); }

    // 取得配置和底层原始图像
    const auto& config = impl_->config;
    const auto* input = frame.buffer.get();

    // 检查图像尺寸、格式和行跨度
    // 先算出每行至少需要多少有效字节
    const int minimum_stride = config.width * (impl_->input_format == AV_PIX_FMT_BGR24 ? 3 : 2);
    if (!input || frame.width != config.width || frame.height != config.height ||
        frame.pixel_format != config.pixel_format || input->width != config.width ||
        input->height != config.height || input->format != impl_->input_format ||
        !input->data[0] || (input->linesize[0] < minimum_stride && input->linesize[0] > -minimum_stride)) {
        throw std::invalid_argument("input frame dimensions, pixel format or stride do not match encoder");
    }
    if (input->format == AV_PIX_FMT_YUYV422 &&
        ((input->color_range != AVCOL_RANGE_UNSPECIFIED && input->color_range != AVCOL_RANGE_MPEG) ||
         (input->colorspace != AVCOL_SPC_UNSPECIFIED && input->colorspace != AVCOL_SPC_SMPTE170M &&
          input->colorspace != AVCOL_SPC_BT470BG))) {
        throw std::invalid_argument("stage 3 YUYV input requires limited-range BT.601 (or unspecified metadata)");
    }
    
    const auto timing = impl_->timing(frame);
    std::vector<H264AccessUnit> output;
    try {
        require_success(av_frame_make_writable(impl_->converted), "make encoder frame writable");
        const int rows = sws_scale(impl_->converter, input->data, input->linesize,
            0, config.height, impl_->converted->data, impl_->converted->linesize);
        if (rows != config.height) { throw std::runtime_error("incomplete encoder pixel conversion"); }
        impl_->converted->pts = timing.pts;
        impl_->converted->duration = timing.duration;
        int sent = avcodec_send_frame(impl_->codec, impl_->converted);
        if (sent == AVERROR(EAGAIN)) {
            impl_->receive(output);
            sent = avcodec_send_frame(impl_->codec, impl_->converted);
        }
        require_success(sent, "send H264 input frame");
        impl_->pending.emplace(timing.pts, timing.duration);
        if (impl_->mode == EncoderTimestampMode::Unset) {
            impl_->mode = frame.source_pts ? EncoderTimestampMode::Source : EncoderTimestampMode::FrameRate;
            if (frame.source_pts) {
                impl_->source_origin = frame.source_pts;
                impl_->source_time_base = {frame.time_base_num, frame.time_base_den};
            }
        }
        if (impl_->mode == EncoderTimestampMode::FrameRate || !frame.source_pts) { ++impl_->fallbacks; }
        impl_->last_pts = timing.pts;
        ++impl_->submitted;
        impl_->receive(output);
    } catch (...) {
        impl_->failed = true;
        throw;
    }
    return output;
}

std::vector<H264AccessUnit> H264Encoder::flush() {
    impl_->require_open();
    if (impl_->flushed) { return {}; }
    std::vector<H264AccessUnit> output;
    try {
        int sent = avcodec_send_frame(impl_->codec, nullptr);
        if (sent == AVERROR(EAGAIN)) {
            impl_->receive(output);
            sent = avcodec_send_frame(impl_->codec, nullptr);
        }
        require_success(sent, "flush H264 encoder");
        if (!impl_->receive(output) || !impl_->pending.empty()) {
            throw std::runtime_error("encoder flush did not return all submitted pictures");
        }
        impl_->flushed = true;
    } catch (...) {
        impl_->failed = true;
        throw;
    }
    return output;
}

void H264Encoder::close() noexcept { impl_->release(); }
const std::string& H264Encoder::sps() const { return impl_->sps; }
const std::string& H264Encoder::pps() const { return impl_->pps; }
EncoderTimestampMode H264Encoder::timestamp_mode() const { return impl_->mode; }
std::uint64_t H264Encoder::timestamp_fallbacks() const { return impl_->fallbacks; }

}
