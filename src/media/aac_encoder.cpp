#include "media/aac_encoder.h"
#include "media/audio_ffmpeg.h"
#include "media/audio_resampler.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/intreadwrite.h>
}

#include <algorithm>
#include <limits>
#include <optional>

namespace rtsp {

struct AacEncoder::Impl {
    AVCodecContext* codec = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVPacket* packet = nullptr;
    AudioResampler resampler;
    MediaTrack track;
    AacEncoderStats stats;
    std::optional<std::int64_t> last_pts;
    bool flushed = false;

    ~Impl() {
        close();
    }

    void close() noexcept {
        av_packet_free(&packet);
        av_audio_fifo_free(fifo);
        fifo = nullptr;
        avcodec_free_context(&codec);
        resampler.close();
    }

    void receive(std::vector<AacAccessUnit>& output, bool draining = false) {
        while (true) {
            const int result = avcodec_receive_packet(codec, packet);
            if (result == AVERROR_EOF) {
                return;
            }
            if (result == AVERROR(EAGAIN) && !draining) {
                return;
            }
            detail::check_audio(result, "receive AAC packet");
            if (packet->pts == AV_NOPTS_VALUE || (last_pts && packet->pts <= *last_pts) ||
                packet->size <= 0 || packet->size > 8191 || packet->duration <= 0 ||
                packet->duration > std::numeric_limits<std::uint32_t>::max()) {
                av_packet_unref(packet);
                throw std::runtime_error("invalid AAC packet size, PTS or duration");
            }
            output.push_back({std::string(reinterpret_cast<const char*>(packet->data), packet->size),
                              packet->pts, static_cast<std::uint32_t>(packet->duration)});
            last_pts = packet->pts;
            std::size_t side_size = 0;
            const auto* side = av_packet_get_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, &side_size);
            if (side && side_size >= 10) {
                stats.skip_samples += AV_RL32(side);
                stats.discard_padding += AV_RL32(side + 4);
            }
            ++stats.packets;
            av_packet_unref(packet);
        }
    }

    void send(AVFrame* frame, std::vector<AacAccessUnit>& output) {
        int result = avcodec_send_frame(codec, frame);
        if (result == AVERROR(EAGAIN)) {
            receive(output);
            result = avcodec_send_frame(codec, frame);
        }
        detail::check_audio(result, "send AAC frame");
        receive(output, frame == nullptr);
    }

    void append(const std::shared_ptr<AVFrame>& frame) {
        if (!frame) {
            return;
        }
        const int size = av_audio_fifo_size(fifo);
        if (frame->nb_samples > std::numeric_limits<int>::max() - size) {
            throw std::runtime_error("PCM FIFO capacity exceeded");
        }
        detail::check_audio(av_audio_fifo_realloc(fifo, size + frame->nb_samples), "grow PCM FIFO");
        if (av_audio_fifo_write(fifo, reinterpret_cast<void**>(frame->extended_data),
                                frame->nb_samples) != frame->nb_samples) {
            throw std::runtime_error("incomplete PCM FIFO write");
        }
        stats.resampled_samples += frame->nb_samples;
    }

    void encode_fifo(std::vector<AacAccessUnit>& output, bool tail) {
        while (av_audio_fifo_size(fifo) >= codec->frame_size ||
               (tail && av_audio_fifo_size(fifo) > 0)) {
            auto frame = detail::audio_buffer(codec->sample_rate, 1, codec->sample_fmt, codec->frame_size);
            detail::check_audio(av_samples_set_silence(frame->extended_data, 0, frame->nb_samples,
                                                      1, codec->sample_fmt), "clear AAC input");
            const int count = std::min(av_audio_fifo_size(fifo), codec->frame_size);
            if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(frame->extended_data), count) != count) {
                throw std::runtime_error("incomplete PCM FIFO read");
            }
            frame->pts = stats.submitted_samples;
            frame->duration = frame->nb_samples;
            send(frame.get(), output);
            stats.submitted_samples += frame->nb_samples;
            stats.tail_padding += frame->nb_samples - count;
        }
    }
};

AacEncoder::AacEncoder() : impl_(std::make_unique<Impl>()) {}
AacEncoder::~AacEncoder() = default;

void AacEncoder::open(const AacEncoderConfig& config) {
    close();
    if (config.bitrate < 8000 || config.bitrate > 256000) {
        throw std::invalid_argument("AAC bitrate must be between 8000 and 256000");
    }
    try {
        const auto* encoder = avcodec_find_encoder_by_name("aac");
        if (!encoder) {
            throw std::runtime_error("FFmpeg native AAC encoder unavailable");
        }
        impl_->codec = avcodec_alloc_context3(encoder);
        impl_->packet = av_packet_alloc();
        if (!impl_->codec || !impl_->packet) {
            throw std::bad_alloc();
        }
        const void* formats = nullptr;
        int count = 0;
        detail::check_audio(avcodec_get_supported_config(impl_->codec, encoder,
            AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &formats, &count), "query AAC sample formats");
        if (!formats || count <= 0) {
            throw std::runtime_error("AAC has no supported sample format");
        }
        auto* codec = impl_->codec;
        codec->sample_fmt = static_cast<const AVSampleFormat*>(formats)[0];
        codec->sample_rate = 48000;
        codec->time_base = {1, 48000};
        codec->bit_rate = config.bitrate;
        codec->profile = AV_PROFILE_AAC_LOW;
        codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_channel_layout_default(&codec->ch_layout, 1);
        detail::check_audio(avcodec_open2(codec, encoder, nullptr), "open AAC encoder");
        if (codec->frame_size != 1024 || codec->sample_rate != 48000 ||
            codec->ch_layout.nb_channels != 1 || !codec->extradata || codec->extradata_size <= 0) {
            throw std::runtime_error("AAC requires 48 kHz mono, 1024 samples and AudioSpecificConfig");
        }
        impl_->fifo = av_audio_fifo_alloc(codec->sample_fmt, 1, codec->frame_size);
        if (!impl_->fifo) {
            throw std::bad_alloc();
        }
        impl_->resampler.open(codec->sample_rate, av_get_sample_fmt_name(codec->sample_fmt));
        std::string config_hex;
        const char* hex = "0123456789abcdef";
        for (int index = 0; index < codec->extradata_size; ++index) {
            config_hex += hex[codec->extradata[index] >> 4];
            config_hex += hex[codec->extradata[index] & 15];
        }
        impl_->track = {"audio", "trackID=1", 97, "MPEG4-GENERIC", 48000, 1,
                        "streamtype=5;profile-level-id=1;mode=AAC-hbr;config=" + config_hex +
                            ";SizeLength=13;IndexLength=3;IndexDeltaLength=3"};
        impl_->stats.initial_padding = codec->initial_padding;
    } catch (...) {
        close();
        throw;
    }
}

std::vector<AacAccessUnit> AacEncoder::encode(const AudioFrame& input) {
    if (!impl_->codec || impl_->flushed) {
        throw std::logic_error("AAC encoder is closed or flushed");
    }
    if (!input.buffer || input.nb_samples != input.buffer->nb_samples ||
        input.sample_rate != input.buffer->sample_rate || input.channels != input.buffer->ch_layout.nb_channels) {
        throw std::invalid_argument("inconsistent AudioFrame PCM metadata");
    }
    std::vector<AacAccessUnit> output;
    impl_->append(impl_->resampler.convert(*input.buffer));
    impl_->stats.input_samples += input.nb_samples;
    impl_->encode_fifo(output, false);
    return output;
}

std::vector<AacAccessUnit> AacEncoder::flush() {
    if (!impl_->codec) {
        throw std::logic_error("AAC encoder is closed");
    }
    if (impl_->flushed) { return {}; }
    impl_->flushed = true;
    std::vector<AacAccessUnit> output;
    while (auto frame = impl_->resampler.flush()) {
        impl_->append(frame);
    }
    impl_->encode_fifo(output, true);
    impl_->send(nullptr, output);
    impl_->stats.trailing_padding = impl_->codec->trailing_padding;
    return output;
}

void AacEncoder::close() noexcept {
    impl_->close();
    impl_->stats = {};
    impl_->last_pts.reset();
    impl_->flushed = false;
}

const MediaTrack& AacEncoder::track() const {
    if (!impl_->codec) {
        throw std::logic_error("AAC encoder is closed");
    }
    return impl_->track;
}

AacEncoderStats AacEncoder::stats() const {
    return impl_->stats;
}
}
