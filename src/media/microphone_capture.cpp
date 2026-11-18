#include "media/microphone_capture.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/samplefmt.h>
}

#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <stdexcept>

namespace rtsp {
namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

std::int64_t steady_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count();
}

std::string error_text(int result) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof(message));
    return message;
}

void require_success(int result, const std::string& operation) {
    if (result < 0) {
        throw std::runtime_error(operation + ": " + error_text(result));
    }
}

std::string utf8(const wchar_t* text, int length) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("could not convert microphone name to UTF-8");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text, length, result.data(), size, nullptr, nullptr);
    return result;
}

class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE) {
            throw std::runtime_error("could not initialize COM for microphone enumeration");
        }
    }
    ~ComScope() {
        if (SUCCEEDED(result_)) { CoUninitialize(); }
    }
private:
    HRESULT result_;
};

void validate(const MicrophoneConfig& config) {
    if (config.device_name.empty() || config.sample_rate < 0 || config.sample_rate > 384000
        || config.channels < 0 || config.channels > 64
        || config.audio_buffer_ms < 0 || config.audio_buffer_ms > 10000) {
        throw std::invalid_argument("invalid microphone device, sample rate, channels or buffer size");
    }
}

}  // namespace

struct MicrophoneCapture::Impl {
    AVFormatContext* input = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    int stream_index = -1;
    bool packet_pending = false;
    bool failed = false;
    MicrophoneFormat actual;
    std::string error;
    std::uint64_t sequence = 0;

    std::atomic<bool> stop{false};
    const std::atomic<bool>* external_stop = nullptr;
    std::mutex wait_mutex;
    std::condition_variable wake;

    bool stopped() const {
        return stop.load() || (external_stop && external_stop->load());
    }

    static int interrupted(void* opaque) {
        return static_cast<Impl*>(opaque)->stopped() ? 1 : 0;
    }

    MicrophoneReadResult fail(const std::string& message) {
        error = message;
        failed = true;
        return MicrophoneReadResult::Failed;
    }

    MicrophoneReadResult deliver(AudioFrame& frame) {
        if (decoded->nb_samples <= 0 || decoded->sample_rate != actual.sample_rate
            || decoded->format != decoder->sample_fmt
            || av_channel_layout_compare(&decoded->ch_layout, &decoder->ch_layout) != 0) {
            return fail("microphone PCM format changed or frame is invalid");
        }
        AVFrame* retained = av_frame_clone(decoded);
        if (!retained) { throw std::bad_alloc(); }
        frame.buffer = std::shared_ptr<AVFrame>(retained, [](AVFrame* owned) {
            av_frame_free(&owned);
        });
        frame.sequence = sequence++;
        frame.sample_rate = actual.sample_rate;
        frame.channels = actual.channels;
        frame.sample_format = actual.sample_format;
        frame.nb_samples = decoded->nb_samples;
        const auto pts = decoded->pts != AV_NOPTS_VALUE ? decoded->pts : decoded->best_effort_timestamp;
        if (pts != AV_NOPTS_VALUE) { frame.source_pts = pts; }
        const auto time_base = decoded->time_base.num > 0 && decoded->time_base.den > 0
            ? decoded->time_base : AVRational{actual.time_base_num, actual.time_base_den};
        frame.time_base_num = time_base.num;
        frame.time_base_den = time_base.den;
        frame.arrival_steady_us = steady_us();
        av_frame_unref(decoded);
        return MicrophoneReadResult::FrameReady;
    }
};

MicrophoneCapture::MicrophoneCapture() : impl_(std::make_unique<Impl>()) {}
MicrophoneCapture::~MicrophoneCapture() { close(); }

std::string MicrophoneCapture::ffmpeg_version() { return av_version_info(); }

std::vector<MicrophoneDeviceInfo> MicrophoneCapture::enumerate_devices() {
    ComScope scope;
    ComPtr<ICreateDevEnum> device_enum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(device_enum.GetAddressOf())))) {
        throw std::runtime_error("could not create DirectShow device enumerator");
    }
    ComPtr<IEnumMoniker> monikers;
    const HRESULT enumerated = device_enum->CreateClassEnumerator(
        CLSID_AudioInputDeviceCategory, monikers.GetAddressOf(), 0);
    if (enumerated == S_FALSE) { return {}; }
    if (FAILED(enumerated)) {
        throw std::runtime_error("could not enumerate DirectShow audio devices");
    }
    std::vector<MicrophoneDeviceInfo> devices;
    ComPtr<IMoniker> moniker;
    HRESULT next = S_OK;
    while ((next = monikers->Next(1, moniker.ReleaseAndGetAddressOf(), nullptr)) == S_OK) {
        ComPtr<IPropertyBag> properties;
        if (FAILED(moniker->BindToStorage(nullptr, nullptr,
            IID_PPV_ARGS(properties.GetAddressOf())))) { continue; }
        VARIANT friendly;
        VariantInit(&friendly);
        const HRESULT named = properties->Read(L"FriendlyName", &friendly, nullptr);
        std::string description;
        try {
            if (SUCCEEDED(named) && friendly.vt == VT_BSTR && SysStringLen(friendly.bstrVal) > 0) {
                description = utf8(friendly.bstrVal, static_cast<int>(SysStringLen(friendly.bstrVal)));
            }
        } catch (...) {
            VariantClear(&friendly);
            throw;
        }
        VariantClear(&friendly);
        if (description.empty()) { continue; }
        LPOLESTR display_name = nullptr;
        const HRESULT displayed = moniker->GetDisplayName(nullptr, nullptr, &display_name);
        std::string name = description;
        if (SUCCEEDED(displayed) && display_name) {
            try { name = utf8(display_name, static_cast<int>(wcslen(display_name))); }
            catch (...) { CoTaskMemFree(display_name); throw; }
            CoTaskMemFree(display_name);
            std::replace(name.begin(), name.end(), ':', '_');
        }
        devices.push_back({name, description});
    }
    if (FAILED(next)) { throw std::runtime_error("DirectShow device enumeration failed"); }
    return devices;
}

void MicrophoneCapture::open(const MicrophoneConfig& config, const std::atomic<bool>* stop_requested) {
    close();
    validate(config);
    impl_->stop.store(false);
    impl_->external_stop = stop_requested;
    impl_->failed = false;
    impl_->error.clear();
    impl_->sequence = 0;
    AVDictionary* options = nullptr;
    try {
        if (impl_->stopped()) { throw std::runtime_error("microphone open cancelled"); }
        avdevice_register_all();
        const AVInputFormat* device = av_find_input_format("dshow");
        if (!device) { throw std::runtime_error("FFmpeg was built without DirectShow input"); }
        // 0 表示设备默认值，不传对应选项；本阶段不做重采样或声道转换。
        if (config.sample_rate) {
            require_success(av_dict_set_int(&options, "sample_rate", config.sample_rate, 0), "sample_rate");
        }
        if (config.channels) {
            require_success(av_dict_set_int(&options, "channels", config.channels, 0), "channels");
        }
        if (config.audio_buffer_ms) {
            require_success(av_dict_set_int(&options, "audio_buffer_size", config.audio_buffer_ms, 0),
                            "audio_buffer_size");
        }
        impl_->input = avformat_alloc_context();
        if (!impl_->input) { throw std::bad_alloc(); }
        impl_->input->flags |= AVFMT_FLAG_NONBLOCK;
        impl_->input->interrupt_callback = {Impl::interrupted, impl_.get()};
        require_success(avformat_open_input(&impl_->input,
            ("audio=" + config.device_name).c_str(), device, &options), "open microphone");
        if (impl_->stopped()) { throw std::runtime_error("microphone open cancelled"); }
        if (av_dict_count(options) != 0) { throw std::runtime_error("unconsumed microphone options"); }
        av_dict_free(&options);
        for (unsigned int index = 0; index < impl_->input->nb_streams; ++index) {
            if (impl_->input->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                impl_->stream_index = static_cast<int>(index);
                break;
            }
        }
        if (impl_->stream_index < 0) { throw std::runtime_error("microphone has no audio stream"); }
        const AVStream* stream = impl_->input->streams[impl_->stream_index];
        const AVCodecParameters* parameters = stream->codecpar;
        const std::string codec_name = avcodec_get_name(parameters->codec_id);
        if (codec_name.rfind("pcm_", 0) != 0) { throw std::runtime_error("microphone requires PCM input"); }
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) { throw std::runtime_error("microphone PCM decoder unavailable"); }
        impl_->decoder = avcodec_alloc_context3(codec);
        if (!impl_->decoder) { throw std::bad_alloc(); }
        require_success(avcodec_parameters_to_context(impl_->decoder, parameters), "PCM parameters");
        impl_->decoder->pkt_timebase = stream->time_base;
        require_success(avcodec_open2(impl_->decoder, codec, nullptr), "open PCM decoder");
        const auto* decoder = impl_->decoder;
        const char* sample_format = av_get_sample_fmt_name(decoder->sample_fmt);
        if (decoder->sample_rate <= 0 || decoder->ch_layout.nb_channels <= 0 || !sample_format
            || stream->time_base.num <= 0 || stream->time_base.den <= 0) {
            throw std::runtime_error("invalid negotiated microphone format");
        }
        if ((config.sample_rate && decoder->sample_rate != config.sample_rate)
            || (config.channels && decoder->ch_layout.nb_channels != config.channels)) {
            throw std::runtime_error("negotiated microphone format does not match request");
        }
        char layout[128]{};
        require_success(av_channel_layout_describe(&decoder->ch_layout, layout, sizeof(layout)),
                        "microphone channel layout");
        impl_->actual = {decoder->sample_rate, decoder->ch_layout.nb_channels, sample_format,
                        layout, codec_name, stream->time_base.num, stream->time_base.den};
        impl_->packet = av_packet_alloc();
        impl_->decoded = av_frame_alloc();
        if (!impl_->packet || !impl_->decoded) { throw std::bad_alloc(); }
    } catch (const std::exception& error) {
        impl_->error = error.what();
        impl_->failed = true;
        av_dict_free(&options);
        close();
        throw;
    }
}

MicrophoneReadResult MicrophoneCapture::read(AudioFrame& frame, std::chrono::milliseconds timeout) {
    frame = {};
    if (timeout.count() < 0) { throw std::invalid_argument("negative microphone read timeout"); }
    if (impl_->stopped()) { return MicrophoneReadResult::Stopped; }
    if (impl_->failed) { return MicrophoneReadResult::Failed; }
    if (!impl_->input) { return impl_->fail("microphone is not open"); }
    const auto deadline = Clock::now() + timeout;
    bool attempted_read = false;
    for (;;) {
        if (impl_->stopped()) { return MicrophoneReadResult::Stopped; }
        // 先取解码输出，保留尚未被解码器接收的包，避免 EAGAIN 时丢包。
        const int received = avcodec_receive_frame(impl_->decoder, impl_->decoded);
        if (received == 0) { return impl_->deliver(frame); }
        if (received != AVERROR(EAGAIN)) {
            return impl_->fail("receive microphone PCM: " + error_text(received));
        }
        if (impl_->packet_pending) {
            const int sent = avcodec_send_packet(impl_->decoder, impl_->packet);
            if (sent == AVERROR(EAGAIN)) {
                // receive 和 send 同时要求对方推进，违反解码器状态约定，不能忙循环。
                return impl_->fail("microphone PCM decoder made no progress");
            }
            av_packet_unref(impl_->packet);
            impl_->packet_pending = false;
            if (sent < 0) { return impl_->fail("decode microphone PCM: " + error_text(sent)); }
            continue;
        }
        // 零超时也尝试读取一次；连续无输出时仍遵守本次读取截止时间。
        if (attempted_read && Clock::now() >= deadline) { return MicrophoneReadResult::Timeout; }
        attempted_read = true;
        const int result = av_read_frame(impl_->input, impl_->packet);
        if (impl_->stopped()) {
            av_packet_unref(impl_->packet);
            return MicrophoneReadResult::Stopped;
        }
        if (result == AVERROR(EAGAIN)) {
            if (Clock::now() >= deadline) { return MicrophoneReadResult::Timeout; }
            std::unique_lock<std::mutex> lock(impl_->wait_mutex);
            impl_->wake.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(5)),
                [this] { return impl_->stopped(); });
        } else if (result < 0) {
            return impl_->fail("read microphone (device lost or input failed): " + error_text(result));
        } else if (impl_->packet->stream_index == impl_->stream_index) {
            impl_->packet_pending = true;
        } else {
            av_packet_unref(impl_->packet);
        }
    }
}

void MicrophoneCapture::request_stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->wait_mutex);
        impl_->stop.store(true);
    }
    impl_->wake.notify_all();
}

void MicrophoneCapture::close() {
    av_frame_free(&impl_->decoded);
    av_packet_free(&impl_->packet);
    avcodec_free_context(&impl_->decoder);
    avformat_close_input(&impl_->input);
    impl_->external_stop = nullptr;
    impl_->stream_index = -1;
    impl_->packet_pending = false;
    impl_->actual = {};
}

const MicrophoneFormat& MicrophoneCapture::format() const { return impl_->actual; }
const std::string& MicrophoneCapture::last_error() const { return impl_->error; }

}  // namespace rtsp
