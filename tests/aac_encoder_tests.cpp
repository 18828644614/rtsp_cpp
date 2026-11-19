#include "support/synthetic_audio_input.h"
#include "media/aac_encoder.h"
#include "media/audio_resampler.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

void check(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

template<class Function> void rejects(Function action) {
    try {
        action();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("accepted invalid audio operation");
}

class Decoder {
public:
    explicit Decoder(const rtsp::MediaTrack& track) {
        try {
            context_ = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_AAC));
            frame_ = av_frame_alloc();
            packet_ = av_packet_alloc();
            if (!context_ || !frame_ || !packet_) {
                throw std::bad_alloc();
            }
            const auto start = track.fmtp.find("config=") + 7;
            const auto config = track.fmtp.substr(start, track.fmtp.find(';', start) - start);
            context_->extradata_size = static_cast<int>(config.size() / 2);
            context_->extradata = static_cast<std::uint8_t*>(av_mallocz(config.size() / 2 + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!context_->extradata) {
                throw std::bad_alloc();
            }
            for (int index = 0; index < context_->extradata_size; ++index) {
                context_->extradata[index] = static_cast<std::uint8_t>(std::stoi(config.substr(index * 2, 2), nullptr, 16));
            }
            rtsp::detail::check_audio(avcodec_open2(context_, avcodec_find_decoder(AV_CODEC_ID_AAC), nullptr), "open test decoder");
        } catch (...) {
            close();
            throw;
        }
    }

    ~Decoder() {
        close();
    }

    void feed(const rtsp::AacAccessUnit& unit) {
        rtsp::detail::check_audio(av_new_packet(packet_, static_cast<int>(unit.data.size())), "allocate test packet");
        std::memcpy(packet_->data, unit.data.data(), unit.data.size());
        packet_->pts = unit.pts;
        rtsp::detail::check_audio(avcodec_send_packet(context_, packet_), "decode AAC");
        av_packet_unref(packet_);
        receive();
    }

    void finish() {
        rtsp::detail::check_audio(avcodec_send_packet(context_, nullptr), "flush test decoder");
        receive();
    }

    std::vector<float> samples;

private:
    void receive() {
        while (true) {
            const int result = avcodec_receive_frame(context_, frame_);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }
            rtsp::detail::check_audio(result, "receive decoded AAC");
            check(frame_->sample_rate == 48000 && frame_->ch_layout.nb_channels == 1 &&
                  frame_->format == AV_SAMPLE_FMT_FLTP, "unexpected decoded AAC format");
            const auto* data = reinterpret_cast<const float*>(frame_->extended_data[0]);
            samples.insert(samples.end(), data, data + frame_->nb_samples);
            av_frame_unref(frame_);
        }
    }

    void close() {
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&context_);
    }

    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
};

void encode_case(std::int64_t count, int rate, int channels) {
    rtsp::AacEncoder encoder;
    encoder.open();
    const auto track = encoder.track();
    check(track.clock_rate == 48000 && track.channels == 1 && track.encoding == "MPEG4-GENERIC",
          "invalid encoder track");
    rtsp::SessionDescription description;
    description.tracks = {track};
    check(rtsp::parse_sdp(rtsp::make_sdp(description)).tracks[0].fmtp == track.fmtp,
          "AudioSpecificConfig did not survive SDP");
    std::vector<rtsp::AacAccessUnit> units;
    const auto append = [&](std::vector<rtsp::AacAccessUnit> output) {
        units.insert(units.end(), std::make_move_iterator(output.begin()), std::make_move_iterator(output.end()));
    };
    rtsp::test::SyntheticAudioInput input(count, rate, channels);
    if (count > 0) {
        rtsp::AudioFrame frame;
        while (input.read(frame, std::chrono::milliseconds(0)) == rtsp::AudioInputResult::FrameReady) {
            append(encoder.encode(frame));
        }
    }
    append(encoder.flush());
    check(encoder.flush().empty(), "repeated AAC flush returned packets");
    const auto stats = encoder.stats();
    check(stats.input_samples == count, "input sample count changed");
    const auto expected = (count * 48000 + rate - 1) / rate;
    check(std::abs(stats.resampled_samples - expected) <= 1, "resampler lost delayed samples");
    check(stats.submitted_samples == stats.resampled_samples + stats.tail_padding &&
          stats.submitted_samples % 1024 == 0 && stats.tail_padding >= 0 && stats.tail_padding < 1024,
          "tail padding or FIFO sample accounting failed");
    Decoder decoder(track);
    for (std::size_t index = 0; index < units.size(); ++index) {
        check(units[index].duration == 1024, "unexpected full-frame AAC duration");
        check(units[index].pts == -stats.initial_padding + static_cast<std::int64_t>(index) * 1024,
              "AAC packet PTS is discontinuous or initial padding was overwritten");
        decoder.feed(units[index]);
    }
    decoder.finish();
    const auto decoded = static_cast<std::int64_t>(decoder.samples.size());
    check(decoded == (count ? stats.submitted_samples + stats.initial_padding : 0),
          "decoded sample count does not match padding accounting");
    if (count > rate / 4 + 100) {
        const auto peak = std::max_element(decoder.samples.begin(), decoder.samples.end(),
                                           [](float first, float second) {
                                               return std::abs(first) < std::abs(second);
                                           });
        const auto position = std::distance(decoder.samples.begin(), peak);
        check(std::abs(position - (stats.initial_padding + 12000)) <= 24,
              "decoded pulse position disagrees with PCM PTS and initial padding");
        check(std::abs(*peak) > 0.1f, "decoded pulse disappeared");
        std::cout << "pulse input=" << rate / 4 << '/' << rate << " decoded=" << position
                  << "/48000 expected=" << stats.initial_padding + 12000 << '\n';
    }
    std::cout << "samples input=" << count << " rate=" << rate << " channels=" << channels
              << " resampled=" << stats.resampled_samples << " submitted=" << stats.submitted_samples
              << " initial_padding=" << stats.initial_padding << " tail_padding=" << stats.tail_padding
              << " decoded=" << decoded << " packets=" << units.size() << '\n';
    rejects([&] { encoder.encode({}); });
    encoder.close();
    rejects([&] {
        encoder.track();
    });
    encoder.open();
    encoder.flush();
}

void formats_and_errors() {
    rtsp::AudioResampler converter;
    rejects([&] {
        converter.open(48000, "invalid");
    });
    converter.open(48000, "fltp");
    auto planar = rtsp::detail::audio_buffer(48000, 2, AV_SAMPLE_FMT_S16P, 1500);
    av_samples_set_silence(planar->extended_data, 0, 1500, 2, AV_SAMPLE_FMT_S16P);
    check(converter.convert(*planar)->nb_samples == 1500, "planar PCM conversion failed");
    auto different = rtsp::detail::audio_buffer(44100, 2, AV_SAMPLE_FMT_S16P, 100);
    rejects([&] {
        converter.convert(*different);
    });
    while (converter.flush()) {}
    check(!converter.flush(), "repeated resampler flush produced data");
    rejects([&] {
        converter.convert(*planar);
    });
    rtsp::AacEncoder encoder;
    rejects([&] {
        encoder.flush();
    });
    rejects([&] { encoder.open({0}); });
    encoder.open();
    rejects([&] { encoder.encode({}); });
}

}

int main() {
    try {
        for (const int count : {0, 1, 1023, 1024, 1025, 1500, 48017}) {
            encode_case(count, 48000, 1);
        }
        encode_case(44117, 44100, 2);
        encode_case(32017, 32000, 2);
        encode_case(96017, 96000, 2);
        formats_and_errors();
        std::cout << "PASS: resampling, FIFO boundaries, AAC, ASC, signed PTS, padding and decoded pulse\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
