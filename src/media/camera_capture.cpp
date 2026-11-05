#include "media/camera_capture.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
        throw std::runtime_error("could not convert camera name to UTF-8");
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
            throw std::runtime_error("could not initialize COM for camera enumeration");
        }
    }
    ~ComScope() {
        if (SUCCEEDED(result_)) { CoUninitialize(); }
    }
private:
    HRESULT result_;
};

void validate(const CameraConfig& config) {
    if (config.device_name.empty() || config.width <= 0 || config.width > 16384 ||
        config.height <= 0 || config.height > 16384 || config.fps_num <= 0 ||
        config.fps_den <= 0 || av_q2d({config.fps_num, config.fps_den}) > 240.0) {
        throw std::invalid_argument("invalid camera device, dimensions or frame rate");
    }
    if (config.pixel_format != "yuyv422" && config.pixel_format != "bgr24") {
        throw std::invalid_argument("stage 2 supports raw yuyv422 or bgr24 input only");
    }
}

}

struct CameraCapture::Impl {
    AVFormatContext* input = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    int stream_index = -1;
    CameraFormat actual;
    std::string error;
    std::atomic<bool> stop{false};
    std::mutex wait_mutex;
    std::condition_variable wake;
    bool failed = false;
    std::uint64_t sequence = 0;
    std::optional<std::int64_t> first_pts;

    static int interrupted(void* opaque) {
        return static_cast<Impl*>(opaque)->stop.load() ? 1 : 0;
    }

    CaptureResult fail(const std::string& message) {
        error = message;
        failed = true;
        return CaptureResult::Failed;
    }

    void deliver(CameraFrame& frame) {
        AVFrame* retained = av_frame_clone(decoded);
        if (!retained) { throw std::bad_alloc(); }
        frame.buffer = std::shared_ptr<AVFrame>(retained, [](AVFrame* owned) {
            av_frame_free(&owned);
        });
        frame.sequence = sequence++;
        frame.width = decoded->width;
        frame.height = decoded->height;
        const char* format_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(decoded->format));
        frame.pixel_format = format_name ? format_name : "unknown";
        std::copy_n(decoded->linesize, frame.linesize.size(), frame.linesize.begin());
        frame.time_base_num = actual.time_base_num;
        frame.time_base_den = actual.time_base_den;
        frame.source_duration = decoded->duration;
        frame.arrival_steady_us = steady_us();
        const auto timestamp = decoded->best_effort_timestamp != AV_NOPTS_VALUE
            ? decoded->best_effort_timestamp : decoded->pts;
        if (timestamp != AV_NOPTS_VALUE) {
            frame.source_pts = timestamp;
            if (!first_pts) { first_pts = timestamp; }
            frame.relative_pts_us = av_rescale_q(timestamp - *first_pts,
                {actual.time_base_num, actual.time_base_den}, {1, 1000000});
        }
        av_frame_unref(decoded);
    }
};

CameraCapture::CameraCapture() : impl_(std::make_unique<Impl>()) {}
CameraCapture::~CameraCapture() { close(); }

std::string CameraCapture::ffmpeg_version() { return av_version_info(); }

std::vector<CameraDeviceInfo> CameraCapture::enumerate_devices() {
    ComScope scope;
    ComPtr<ICreateDevEnum> device_enum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(device_enum.GetAddressOf())))) {
        throw std::runtime_error("could not create DirectShow device enumerator");
    }
    ComPtr<IEnumMoniker> monikers;
    const HRESULT enumerated = device_enum->CreateClassEnumerator(
        CLSID_VideoInputDeviceCategory, monikers.GetAddressOf(), 0);
    if (enumerated == S_FALSE) { return {}; }
    if (FAILED(enumerated)) {
        throw std::runtime_error("could not enumerate DirectShow video devices");
    }
    std::vector<CameraDeviceInfo> devices;
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

void CameraCapture::open(const CameraConfig& config) {
    close();
    validate(config);
    impl_->stop.store(false);
    impl_->failed = false;
    impl_->error.clear();
    impl_->sequence = 0;
    impl_->first_pts.reset();
    avdevice_register_all();
    const AVInputFormat* device = av_find_input_format("dshow");
    if (!device) { throw std::runtime_error("FFmpeg was built without DirectShow input"); }
    AVDictionary* options = nullptr;
    try {
        require_success(av_dict_set(&options, "video_size",
            (std::to_string(config.width) + "x" + std::to_string(config.height)).c_str(), 0), "video_size");
        require_success(av_dict_set(&options, "framerate",
            (std::to_string(config.fps_num) + "/" + std::to_string(config.fps_den)).c_str(), 0), "framerate");
        require_success(av_dict_set(&options, "pixel_format", config.pixel_format.c_str(), 0), "pixel_format");
        impl_->input = avformat_alloc_context();
        if (!impl_->input) { throw std::bad_alloc(); }
        impl_->input->flags |= AVFMT_FLAG_NONBLOCK;
        impl_->input->interrupt_callback = {Impl::interrupted, impl_.get()};
        require_success(avformat_open_input(&impl_->input,
            ("video=" + config.device_name).c_str(), device, &options), "open camera");
        if (av_dict_count(options) != 0) {
            throw std::runtime_error("FFmpeg did not consume all camera options");
        }
        av_dict_free(&options);
        for (unsigned int index = 0; index < impl_->input->nb_streams; ++index) {
            if (impl_->input->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                impl_->stream_index = static_cast<int>(index);
                break;
            }
        }
        if (impl_->stream_index < 0) { throw std::runtime_error("camera has no video stream"); }
        AVStream* stream = impl_->input->streams[impl_->stream_index];
        const AVCodecParameters* parameters = stream->codecpar;
        if (parameters->codec_id != AV_CODEC_ID_RAWVIDEO) {
            throw std::runtime_error("stage 2 requires a rawvideo camera mode");
        }
        const auto pixel_format = static_cast<AVPixelFormat>(parameters->format);
        const AVRational rate = av_guess_frame_rate(impl_->input, stream, nullptr);
        if (parameters->width != config.width || parameters->height != config.height ||
            pixel_format != av_get_pix_fmt(config.pixel_format.c_str()) ||
            rate.num <= 0 || rate.den <= 0 ||
            std::abs(av_q2d(rate) / av_q2d({config.fps_num, config.fps_den}) - 1.0) > 0.001 ||
            stream->time_base.num <= 0 || stream->time_base.den <= 0) {
            throw std::runtime_error("negotiated camera format does not match request");
        }
        impl_->actual = {parameters->width, parameters->height, rate.num, rate.den,
            av_get_pix_fmt_name(pixel_format), avcodec_get_name(parameters->codec_id),
            stream->time_base.num, stream->time_base.den};
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) { throw std::runtime_error("rawvideo decoder unavailable"); }
        impl_->decoder = avcodec_alloc_context3(codec);
        if (!impl_->decoder) { throw std::bad_alloc(); }
        require_success(avcodec_parameters_to_context(impl_->decoder, parameters), "decoder parameters");
        impl_->decoder->pkt_timebase = stream->time_base;
        require_success(avcodec_open2(impl_->decoder, codec, nullptr), "open rawvideo decoder");
        impl_->packet = av_packet_alloc();
        impl_->decoded = av_frame_alloc();
        if (!impl_->packet || !impl_->decoded) { throw std::bad_alloc(); }
    } catch (...) {
        av_dict_free(&options);
        close();
        throw;
    }
}

CaptureResult CameraCapture::read(CameraFrame& frame, std::chrono::milliseconds timeout) {
    frame = {};
    if (impl_->stop.load()) { return CaptureResult::Stopped; }
    if (impl_->failed) { return CaptureResult::Failed; }
    if (!impl_->input) { return impl_->fail("camera is not open"); }
    if (timeout.count() < 0) { throw std::invalid_argument("negative capture timeout"); }
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        if (impl_->stop.load()) { return CaptureResult::Stopped; }
        const int received = avcodec_receive_frame(impl_->decoder, impl_->decoded);
        if (received == 0) {
            impl_->deliver(frame);
            return CaptureResult::FrameReady;
        }
        if (received != AVERROR(EAGAIN)) {
            return impl_->fail("receive camera frame: " + error_text(received));
        }
        const int result = av_read_frame(impl_->input, impl_->packet);
        if (impl_->stop.load()) {
            av_packet_unref(impl_->packet);
            return CaptureResult::Stopped;
        }
        if (result == AVERROR(EAGAIN)) {
            if (Clock::now() >= deadline) { return CaptureResult::Timeout; }
            std::unique_lock<std::mutex> lock(impl_->wait_mutex);
            impl_->wake.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(5)),
                [this] { return impl_->stop.load(); });
            continue;
        }
        if (result < 0) { return impl_->fail("read camera (device lost or input failed): " + error_text(result)); }
        if (impl_->packet->stream_index == impl_->stream_index) {
            const int sent = avcodec_send_packet(impl_->decoder, impl_->packet);
            av_packet_unref(impl_->packet);
            if (sent < 0) { return impl_->fail("decode camera packet: " + error_text(sent)); }
        } else {
            av_packet_unref(impl_->packet);
        }
        if (Clock::now() >= deadline) { return CaptureResult::Timeout; }
    }
}

void CameraCapture::request_stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->wait_mutex);
        impl_->stop.store(true);
    }
    impl_->wake.notify_all();
}

void CameraCapture::close() {
    av_frame_free(&impl_->decoded);
    av_packet_free(&impl_->packet);
    avcodec_free_context(&impl_->decoder);
    avformat_close_input(&impl_->input);
    impl_->stream_index = -1;
    impl_->actual = {};
}

const CameraFormat& CameraCapture::format() const { return impl_->actual; }
const std::string& CameraCapture::last_error() const { return impl_->error; }

}
