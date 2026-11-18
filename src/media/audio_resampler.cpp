#include "media/audio_resampler.h"
#include "media/audio_ffmpeg.h"

extern "C" {
#include <libswresample/swresample.h>
}

#include <algorithm>

namespace rtsp {

struct AudioResampler::Impl {
    SwrContext* context = nullptr;
    AVChannelLayout input_layout{};
    int input_rate = 0;
    AVSampleFormat input_format = AV_SAMPLE_FMT_NONE;
    int output_rate = 0;
    AVSampleFormat output_format = AV_SAMPLE_FMT_NONE;
    bool finished = false;

    ~Impl() {
        swr_free(&context);
        av_channel_layout_uninit(&input_layout);
    }

    std::shared_ptr<AVFrame> convert(const AVFrame* input) {
        const int estimate = swr_get_out_samples(context, input ? input->nb_samples : 0);
        detail::check_audio(estimate, "estimate resampler output");
        const int capacity = std::max(1, estimate);
        auto output = detail::audio_buffer(output_rate, 1, output_format, capacity);
        const int count = swr_convert(context, output->extended_data, capacity,
                                     input ? input->extended_data : nullptr,
                                     input ? input->nb_samples : 0);
        detail::check_audio(count, "resample audio");
        output->nb_samples = count;
        return count == 0 ? nullptr : output;
    }
};

AudioResampler::AudioResampler() : impl_(std::make_unique<Impl>()) {}
AudioResampler::~AudioResampler() = default;

void AudioResampler::open(int sample_rate, const std::string& sample_format) {
    close();
    impl_->output_format = av_get_sample_fmt(sample_format.c_str());
    if (sample_rate <= 0 || impl_->output_format == AV_SAMPLE_FMT_NONE) {
        throw std::invalid_argument("invalid resampler output format");
    }
    impl_->output_rate = sample_rate;
}

std::shared_ptr<AVFrame> AudioResampler::convert(const AVFrame& input) {
    if (impl_->output_rate == 0 || impl_->finished) {
        throw std::logic_error("resampler is closed or flushed");
    }
    if (input.sample_rate <= 0 || input.nb_samples <= 0 ||
        !av_channel_layout_check(&input.ch_layout) || !input.extended_data ||
        !input.extended_data[0] || av_get_bytes_per_sample(static_cast<AVSampleFormat>(input.format)) == 0) {
        throw std::invalid_argument("invalid input PCM frame");
    }
    if (!impl_->context) {
        AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_MONO;
        detail::check_audio(swr_alloc_set_opts2(&impl_->context, &output_layout,
            impl_->output_format, impl_->output_rate, &input.ch_layout,
            static_cast<AVSampleFormat>(input.format), input.sample_rate, 0, nullptr), "create resampler");
        detail::check_audio(swr_init(impl_->context), "initialize resampler");
        detail::check_audio(av_channel_layout_copy(&impl_->input_layout, &input.ch_layout), "copy audio layout");
        impl_->input_rate = input.sample_rate;
        impl_->input_format = static_cast<AVSampleFormat>(input.format);
    } else if (input.sample_rate != impl_->input_rate || input.format != impl_->input_format ||
               av_channel_layout_compare(&input.ch_layout, &impl_->input_layout) != 0) {
        throw std::runtime_error("PCM format changed during audio encoding");
    }
    return impl_->convert(&input);
}

std::shared_ptr<AVFrame> AudioResampler::flush() {
    if (impl_->finished || !impl_->context) {
        impl_->finished = true;
        return nullptr;
    }
    auto output = impl_->convert(nullptr);
    if (!output) {
        impl_->finished = true;
    }
    return output;
}

void AudioResampler::close() noexcept {
    swr_free(&impl_->context);
    av_channel_layout_uninit(&impl_->input_layout);
    impl_->input_rate = 0;
    impl_->output_rate = 0;
    impl_->finished = false;
}

}
