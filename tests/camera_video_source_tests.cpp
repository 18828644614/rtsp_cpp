#include "app/synthetic_camera_frame.h"
#include "media/camera_video_source.h"
#include "media/h264_depacketizer.h"
#include "media/media_session.h"
#include "rtsp/rtsp_stream_parser.h"
#ifdef RTSP_TEST_CAMERA_CAPTURE
#include "media/camera_capture_input.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void check(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
template<class Predicate> void await(Predicate done) {
    const auto until = Clock::now() + 3s;
    while (!done()) {
        check(Clock::now() < until, "timed out waiting for producer");
        std::this_thread::sleep_for(1ms);
    }
}
struct InputState {
    std::mutex mutex;
    std::condition_variable wake;
    int credits = 1;
    bool eof = false;
    bool fail = false;
    bool stopped = false;
    bool closed = false;
    std::thread::id reader;
    void update(int more = 0, bool end = false, bool error = false) {
        { std::lock_guard<std::mutex> lock(mutex); credits += more; eof = end; fail = error; }
        wake.notify_all();
    }
};
rtsp::CameraVideoSourceConfig config() {
    rtsp::CameraVideoSourceConfig result;
    result.encoder.width = 64;
    result.encoder.height = 48;
    result.encoder.gop = 3;
    result.startup_timeout = 2s;
    result.read_timeout = 20ms;
    return result;
}
class GeneratedInput final : public rtsp::CameraFrameInput {
public:
    explicit GeneratedInput(std::shared_ptr<InputState> state) : state_(std::move(state)) {}
    rtsp::CameraInputResult read(rtsp::CameraFrame& frame, std::chrono::milliseconds timeout) override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->reader = std::this_thread::get_id();
        state_->wake.wait_for(lock, timeout, [&] {
            return state_->stopped || state_->fail || state_->eof || state_->credits > 0;
        });
        if (state_->stopped) { return rtsp::CameraInputResult::Ended; }
        if (state_->fail) { throw std::runtime_error("simulated device lost"); }
        if (state_->credits == 0) {
            return state_->eof ? rtsp::CameraInputResult::Ended : rtsp::CameraInputResult::Waiting;
        }
        --state_->credits;
        lock.unlock();
        frame = rtsp::app::synthetic_camera_frame(64, 48, "yuyv422", 30, 1, index_++);
        return rtsp::CameraInputResult::FrameReady;
    }
    void request_stop() noexcept override {
        { std::lock_guard<std::mutex> lock(state_->mutex); state_->stopped = true; }
        state_->wake.notify_all();
    }
    void close() noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closed = true;
    }
private:
    std::shared_ptr<InputState> state_;
    std::int64_t index_ = 0;
};
std::unique_ptr<rtsp::CameraVideoSource> source(const std::shared_ptr<InputState>& state,
                                              rtsp::CameraVideoSourceConfig cfg = config()) {
    return std::make_unique<rtsp::CameraVideoSource>(std::make_unique<GeneratedInput>(state), cfg);
}
void check_released(const std::shared_ptr<InputState>& state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    check(state->closed && state->stopped, "input not stopped and closed");
    check(state->reader != std::this_thread::get_id(), "capture ran on consumer thread");
}
void test_source_wait_drain_stop() {
    const auto state = std::make_shared<InputState>();
    auto camera = source(state);
    const auto fmtp = camera->track().fmtp;
    check(fmtp.find("sprop-parameter-sets=") != std::string::npos, "startup did not publish SPS/PPS");
    rtsp::H264AccessUnit frame;
    check(camera->read(frame) && frame.is_keyframe && frame.pts == 0 && frame.duration == 3000,
          "startup did not retain complete IDR/time");
    check(!camera->read(frame) && frame.nalus.empty() && !camera->ended(), "waiting confused with EOF");
    state->update(2, true);
    await([&] { return camera->queue_stats().closed; });
    check(!camera->ended(), "producer marked EOF before consumer drained");
    check(camera->read(frame) && frame.pts == 3000 && !frame.is_keyframe, "first P/time missing");
    check(camera->read(frame) && frame.pts == 6000, "EOF lost final encoded frame");
    check(!camera->read(frame) && camera->ended(), "EOF not latched after empty read");
    check(camera->track().fmtp == fmtp, "track changed concurrently");
    camera->stop(); camera->stop();
    check_released(state);

    const auto blocked = std::make_shared<InputState>();
    auto waiting = source(blocked);
    const auto start = Clock::now();
    waiting.reset();
    check(Clock::now() - start < 1s, "destruction did not interrupt blocked input");
    check_released(blocked);
}
void test_startup_and_runtime_failures() {
    for (int mode = 0; mode < 4; ++mode) {
        const auto state = std::make_shared<InputState>();
        auto cfg = config();
        if (mode != 3) { state->credits = 0; }
        if (mode == 0) { cfg.startup_timeout = 50ms; }
        if (mode == 1) { state->eof = true; }
        if (mode == 2) { state->fail = true; }
        if (mode == 3) { cfg.queue.max_bytes = 1; }
        bool failed = false;
        try { auto unused = source(state, cfg); }
        catch (const std::runtime_error&) { failed = true; }
        check(failed, "startup failure was not reported");
        check_released(state);
    }
    const auto state = std::make_shared<InputState>();
    auto camera = source(state);
    state->update(0, false, true);
    await([&] { return camera->queue_stats().closed; });
    rtsp::H264AccessUnit frame;
    check(camera->read(frame), "runtime error lost buffered IDR");
    bool failed = false;
    try { camera->read(frame); }
    catch (const std::runtime_error& e) { failed = std::string(e.what()) == "simulated device lost"; }
    check(failed, "runtime error not delivered to consumer");
}
void test_overflow_and_resume() {
    const auto state = std::make_shared<InputState>();
    auto cfg = config();
    cfg.queue.max_frames = 2;
    auto camera = source(state, cfg);
    state->update(4); // 0/1 入队，2 溢出丢弃；3(IDR)/4 保留。
    await([&] { return camera->queue_stats().dropped == 3 && camera->queue_stats().frames == 2; });
    rtsp::H264AccessUnit frame;
    check(camera->read(frame) && frame.is_keyframe && frame.pts == 9000, "overflow did not recover on IDR");
    camera->resume(); // 剩下的 4 是 P，必须丢弃。
    check(!camera->read(frame) && !camera->ended(), "resume retained undecodable P");
    state->update(2); // 5 丢弃，6(IDR) 恢复。
    await([&] { return camera->queue_stats().frames == 1; });
    check(camera->read(frame) && frame.is_keyframe && frame.pts == 18000, "resume missed next IDR");
}

struct Peer {
    std::shared_ptr<rtsp::net::OutboundQueue> queue = std::make_shared<rtsp::net::OutboundQueue>();
    std::shared_ptr<rtsp::MediaSubscription> subscription = std::make_shared<rtsp::MediaSubscription>(queue);
    explicit Peer(const rtsp::MediaTrack& track) { subscription->add_track(0, track, {}, 100, 42); }
};

// 从真实 RTP 负载重组 AU，再送 FFmpeg 解码，验证线程→队列→MediaSession 的完整交付。
int decode_output(Peer& peer) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* context = avcodec_alloc_context3(codec);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    check(context && packet && frame, "allocate decoder");
    int count = 0;
    try {
        check(avcodec_open2(context, codec, nullptr) >= 0, "open decoder");
        rtsp::H264Depacketizer depacketizer;
        rtsp::net::OutboundMessage message;
        std::string au;
        bool play = false;
        while (peer.queue->try_pop(message)) {
            if (message.kind == rtsp::net::OutboundKind::RtspResponse) { play = true; continue; }
            if (message.kind != rtsp::net::OutboundKind::Rtp) { continue; }
            check(play, "RTP preceded PLAY");
            rtsp::RtspStreamParser parser;
            parser.append(message.wire.data(), message.wire.size());
            rtsp::RtspStreamItem item;
            check(parser.next(item) && item.is_interleaved, "RTP interleaving invalid");
            const auto rtp = rtsp::parse_rtp_packet(item.payload);
            std::vector<std::string> nalus;
            check(depacketizer.push(rtp, nalus) != rtsp::H264Dropped, "RTP depacketizer dropped NALU");
            for (const auto& nalu : nalus) {
                au.append("\0\0\0\1", 4); au += nalu;
            }
            if (!rtp.marker) { continue; }
            check(av_new_packet(packet, static_cast<int>(au.size())) >= 0, "allocate AU packet");
            std::copy(au.begin(), au.end(), packet->data);
            check(avcodec_send_packet(context, packet) >= 0, "send AU to decoder");
            av_packet_unref(packet);
            au.clear();
            while (avcodec_receive_frame(context, frame) == 0) {
                check(frame->width == 64 && frame->height == 48, "decoded dimensions wrong");
                ++count; av_frame_unref(frame);
            }
        }
        check(avcodec_send_packet(context, nullptr) >= 0, "flush decoder");
        while (avcodec_receive_frame(context, frame) == 0) { ++count; av_frame_unref(frame); }
    } catch (...) {
        av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&context); throw;
    }
    av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&context);
    return count;
}
void test_media_session(bool failure) {
    const auto state = std::make_shared<InputState>();
    auto camera = source(state);
    auto* observer = camera.get();
    rtsp::MediaSession media(std::move(camera));
    Peer peer(media.description().tracks[0]);
    check(media.subscribe(peer.subscription, "PLAY 200") == rtsp::SubscribeResult::Started, "PLAY failed");
    const auto now = Clock::now();
    media.tick(now);
    check(!media.ended() && !peer.queue->closed() && media.next_deadline()
              && *media.next_deadline() > now, "live wait closed session or busy-polled");
    state->update(failure ? 0 : 5, !failure, failure);
    await([&] { return observer->queue_stats().closed; });
    for (int i = 1; i <= 8 && !media.ended(); ++i) { media.tick(now + i * 40ms); }
    check(media.ended() && peer.queue->closed() && peer.subscription->failed() == failure,
          "session did not distinguish normal end and device failure");
    check(decode_output(peer) == (failure ? 1 : 6), "MediaSession RTP did not decode all frames");
    check_released(state);
}
void test_idle_and_session_stop() {
    const auto state = std::make_shared<InputState>();
    auto camera = source(state);
    auto* observer = camera.get();
    rtsp::MediaSession media(std::move(camera));
    media.tick(Clock::now());
    check(!media.next_deadline() && observer->queue_stats().frames == 1, "idle session consumed camera");
    state->update(4);
    await([&] { return observer->queue_stats().frames == 5; });
    Peer peer(media.description().tracks[0]);
    media.subscribe(peer.subscription, "PLAY 200");
    check(observer->queue_stats().frames == 2, "first watcher did not trim to latest IDR");
    media.tick(Clock::now());
    peer.subscription->cancel();
    media.tick(Clock::now());
    check(!media.next_deadline(), "idle live session retained deadline");
    state->update(3);
    await([&] { return observer->queue_stats().frames >= 3; });
    Peer resumed(media.description().tracks[0]);
    media.subscribe(resumed.subscription, "PLAY 200");
    media.tick(Clock::now());
    const auto begin = Clock::now();
    media.stop(); media.stop();
    check(Clock::now() - begin < 1s && observer->ended(), "session stop did not join source");
    check_released(state);
    check(decode_output(resumed) == 1, "resume delivered stale prefetched P frame");
}

#ifdef RTSP_TEST_CAMERA_CAPTURE
void test_capture_adapter_without_device() {
    auto input = rtsp::make_camera_capture_input({});
    input->request_stop();
    bool stopped = false;
    try { rtsp::CameraVideoSource camera(std::move(input), config()); }
    catch (const std::runtime_error&) { stopped = true; }
    check(stopped, "pre-stopped capture adapter opened device or accepted startup");

    rtsp::CameraConfig invalid;
    invalid.width = 0;
    bool rejected = false;
    try {
        rtsp::CameraVideoSource camera(rtsp::make_camera_capture_input(invalid), config());
    } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "capture adapter did not propagate worker open failure");
}
#endif
}

int main() {
    try {
        test_source_wait_drain_stop();
        test_startup_and_runtime_failures();
        test_overflow_and_resume();
        test_media_session(false);
        test_media_session(true);
        test_idle_and_session_stop();
#ifdef RTSP_TEST_CAMERA_CAPTURE
        test_capture_adapter_without_device();
#endif
        std::cout << "PASS: generated capture/encode, live wait/end/failure, overflow, resume, RTP decode and stop\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
