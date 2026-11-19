#include "support/synthetic_camera_frame.h"
#include "support/synthetic_audio_input.h"
#include "media/camera_video_source.h"
#include "support/h264_depacketizer.h"
#include "rtp/rtcp_packet.h"
#include "rtsp/rtsp_server.h"
#include "rtsp/rtsp_session_utils.h"
#include "rtsp/rtsp_stream_parser.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void check(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
template<class Predicate> void await(Predicate done) {
    const auto deadline = Clock::now() + 3s;
    while (!done()) {
        check(Clock::now() < deadline, "timed out waiting for server/input");
        std::this_thread::sleep_for(1ms);
    }
}

struct InputState {
    std::mutex mutex;
    std::condition_variable wake;
    int limit = 1;
    bool ended = false;
    bool failed = false;
    bool stopped = false;
    std::atomic<bool> closed{false};
    std::atomic<int> produced{0};
    void allow(int frames, bool end = false, bool fail = false) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            limit = frames;
            ended = end;
            failed = fail;
        }
        wake.notify_all();
    }
};

// 第一帧用于源启动握手，后续帧既受测试许可控制，也按真实帧率推进。
class PacedInput final : public rtsp::CameraFrameInput {
public:
    PacedInput(std::shared_ptr<InputState> state, rtsp::H264EncoderConfig config, bool noise)
        : state_(std::move(state)), config_(config), noise_(noise) {}
    rtsp::CameraInputResult read(rtsp::CameraFrame& frame, std::chrono::milliseconds timeout) override {
        const auto deadline = Clock::now() + timeout;
        std::unique_lock<std::mutex> lock(state_->mutex);
        for (;;) {
            if (state_->stopped) {
                return rtsp::CameraInputResult::Ended;
            }
            if (state_->failed) {
                throw std::runtime_error("simulated camera disconnected");
            }
            if (index_ >= state_->limit && state_->ended) {
                return rtsp::CameraInputResult::Ended;
            }
            if (index_ < state_->limit && Clock::now() >= due_) {
                break;
            }
            if (Clock::now() >= deadline) {
                return rtsp::CameraInputResult::Waiting;
            }
            state_->wake.wait_until(lock, index_ < state_->limit ? std::min(deadline, due_) : deadline);
        }
        lock.unlock();
        frame = rtsp::test::synthetic_camera_frame(config_.width, config_.height,
            config_.pixel_format, config_.fps_num, 1, index_);
        frame.source_pts.reset();
        frame.arrival_steady_us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count();
        if (noise_) {
            std::uint32_t bits = 1234567u + static_cast<std::uint32_t>(index_);
            for (int y = 0; y < config_.height; ++y) {
                auto* row = frame.buffer->data[0] + y * frame.buffer->linesize[0];
                for (int x = 0; x < config_.width * 2; ++x) {
                    bits ^= bits << 13;
                    bits ^= bits >> 17;
                    bits ^= bits << 5;
                    row[x] = static_cast<std::uint8_t>(bits);
                }
            }
        }
        ++index_;
        state_->produced.store(index_);
        due_ = Clock::now() + std::chrono::nanoseconds(1000000000 / config_.fps_num);
        return rtsp::CameraInputResult::FrameReady;
    }
    void request_stop() noexcept override {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->stopped = true;
        }
        state_->wake.notify_all();
    }
    void close() noexcept override {
        state_->closed.store(true);
    }

private:
    std::shared_ptr<InputState> state_;
    const rtsp::H264EncoderConfig config_;
    const bool noise_;
    int index_ = 0;
    Clock::time_point due_ = Clock::now();
};

rtsp::CameraVideoSourceConfig configuration(bool noise = false) {
    rtsp::CameraVideoSourceConfig config;
    config.encoder.width = noise ? 640 : 160;
    config.encoder.height = noise ? 360 : 96;
    config.encoder.gop = 6;
    config.encoder.bitrate = noise ? 80000000 : 500000;
    config.startup_timeout = 3s;
    config.read_timeout = 10ms;
    return config;
}
std::unique_ptr<rtsp::CameraVideoSource> video_source(const std::shared_ptr<InputState>& state,
    bool noise, rtsp::CaptureTimeline timeline) {
    auto config = configuration(noise);
    config.timeline = timeline;
    return std::make_unique<rtsp::CameraVideoSource>(
        std::make_unique<PacedInput>(state, config.encoder, noise), config);
}

std::unique_ptr<rtsp::MicrophoneAudioSource> audio_source(rtsp::CaptureTimeline timeline) {
    rtsp::MicrophoneAudioSourceConfig config;
    config.timeline = timeline;
    return std::make_unique<rtsp::MicrophoneAudioSource>(
        std::make_unique<rtsp::test::SyntheticAudioInput>(0, 48000, 1, true), config);
}

struct Server {
    rtsp::RtspServer server;
    std::thread worker;
    std::exception_ptr failure;
    explicit Server(const std::shared_ptr<InputState>& state, bool noise = false,
                    rtsp::CaptureTimeline timeline = rtsp::CaptureTimeline{})
        : server("127.0.0.1", 0, video_source(state, noise, timeline), audio_source(timeline),
                 timeline, 150ms, runtime()) {
        worker = std::thread([this] {
            try {
                server.run();
            } catch (...) {
                failure = std::current_exception();
            }
        });
        try {
            await([&] {
                return server.local_port() != 0;
            });
        } catch (...) {
            server.stop();
            worker.join();
            throw;
        }
    }
    static rtsp::RuntimeOptions runtime() {
        rtsp::RuntimeOptions options;
        options.rtcp_interval = 50ms;
        options.send_stall_timeout = 500ms;
        return options;
    }
    void stop() {
        server.stop();
        if (worker.joinable()) {
            worker.join();
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    ~Server() {
        server.stop();
        if (worker.joinable()) {
            worker.join();
        }
    }
};

// 独立 TCP 客户端：解析线上 RTSP/RTP/RTCP，重组每个 AU，并交给真正的 H.264 解码器。
class Peer {
public:
    rtsp::net::TcpSocket socket;
    bool eof = false;
    bool played = false;
    bool fragmented = false;
    int units = 0;
    int audio_units = 0;
    int byes = 0;
    std::vector<std::uint64_t> pictures;
    std::vector<std::uint32_t> timestamps;
    std::uint32_t ssrc = 0;
    std::string fmtp;

    Peer(std::uint16_t port, bool slow = false, int width = 160, int height = 96)
        : width_(width), height_(height) {
        socket.connect("127.0.0.1", port);
        if (slow) {
            int bytes = 1024;
            check(setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVBUF,
                reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0, "set receive buffer failed");
        }
        decoder_ = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!decoder_ || !packet_ || !frame_) {
            release();
            throw std::bad_alloc();
        }
        decoder_->err_recognition = AV_EF_EXPLODE;
        if (avcodec_open2(decoder_, decoder_->codec, nullptr) < 0) {
            release();
            throw std::runtime_error("open network decoder failed");
        }
    }
    ~Peer() {
        release();
    }
    rtsp::RtspMessage exchange(const std::string& method, const std::string& suffix = "",
                               const std::string& headers = "", int expected = 200) {
        const auto wire = method + " rtsp://127.0.0.1/live" + suffix + " RTSP/1.0\r\nCSeq: "
            + std::to_string(++cseq_) + "\r\n" + headers + "\r\n";
        socket.send_all(wire.data(), wire.size());
        const auto deadline = Clock::now() + 3s;
        for (;;) {
            rtsp::RtspStreamItem item;
            while (parser_.next(item)) {
                if (item.is_interleaved) {
                    consume(item);
                    continue;
                }
                check(item.message.status_code == expected, "RTSP status mismatch");
                check(item.message.header("CSeq") == std::to_string(cseq_), "CSeq mismatch");
                if (method == "PLAY" && expected == 200) {
                    played = true;
                }
                return item.message;
            }
            check(Clock::now() < deadline && !eof, "RTSP response timed out");
            receive(20);
        }
    }
    void setup() {
        const auto description = exchange("DESCRIBE");
        const auto sdp = rtsp::parse_sdp(description.body);
        check(sdp.tracks.size() == 2 && sdp.tracks[0].clock_rate == 90000,
              "camera SDP track mismatch");
        fmtp = sdp.tracks[0].fmtp;
        check(fmtp.find("profile-level-id=") != std::string::npos
              && fmtp.find("sprop-parameter-sets=") != std::string::npos, "missing camera SPS/PPS in SDP");
        const auto response = exchange("SETUP", "/trackID=0",
                                       "Transport: RTP/AVP/TCP;unicast;interleaved=6-7\r\n");
        session_ = "Session: " + rtsp::session_id_from_header(response.header("Session")) + "\r\n";
        const auto& audio = sdp.tracks[1];
        check(audio.encoding == "MPEG4-GENERIC" && audio.clock_rate == 48000 && audio.channels == 1,
              "live AAC SDP mismatch");
        exchange("SETUP", "/trackID=1", session_ +
                 "Transport: RTP/AVP/TCP;unicast;interleaved=8-9\r\n");
    }
    void play(int expected = 200) {
        const auto response = exchange("PLAY", "", session_ + "Range: npt=0.000-\r\n", expected);
        if (expected == 200) {
            check(response.header("RTP-Info").find("seq=100") != std::string::npos, "wrong RTP-Info sequence");
        }
    }
    void keepalive() {
        exchange("GET_PARAMETER", "", session_);
    }
    void teardown() {
        exchange("TEARDOWN", "", session_);
    }
    void pump(int timeout = 1) {
        rtsp::RtspStreamItem item;
        while (parser_.next(item)) {
            consume(item);
        }
        if (!eof) {
            receive(timeout);
        }
        while (parser_.next(item)) {
            consume(item);
        }
    }
    void wait_units(int count, int audio_count = 0) {
        const auto deadline = Clock::now() + 4s;
        while (units < count || audio_units < audio_count) {
            check(Clock::now() < deadline && !eof, "camera frames timed out");
            pump(10);
        }
    }
    void finish() {
        parser_.finish();
        depacketizer_.finish();
        check(au_.empty(), "stream ended inside an access unit");
        check(avcodec_send_packet(decoder_, nullptr) >= 0, "flush network decoder failed");
        decode_frames();
        check(pictures.size() == static_cast<std::size_t>(units), "network decoder lost frames");
    }
private:
    void release() {
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&decoder_);
    }
    void receive(int timeout) {
        if (!socket.wait_readable(timeout)) {
            return;
        }
        char buffer[65536];
        const auto count = socket.receive(buffer, sizeof(buffer));
        if (!count) {
            eof = true;
        } else {
            parser_.append(buffer, count);
        }
    }
    void decode_frames() {
        for (;;) {
            const int result = avcodec_receive_frame(decoder_, frame_);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }
            check(result == 0 && !(frame_->flags & AV_FRAME_FLAG_CORRUPT), "network video decode failed");
            check(frame_->width == width_ && frame_->height == height_, "decoded network dimensions mismatch");
            std::uint64_t hash = 14695981039346656037ull;
            for (int plane = 0; plane < 3; ++plane) {
                for (int y = 0; y < (plane ? height_ / 2 : height_); ++y) {
                    const auto* row = frame_->data[plane] + y * frame_->linesize[plane];
                    for (int x = 0; x < (plane ? width_ / 2 : width_); ++x) {
                        hash = (hash ^ row[x]) * 1099511628211ull;
                    }
                }
            }
            pictures.push_back(hash);
            av_frame_unref(frame_);
        }
    }
    void consume(const rtsp::RtspStreamItem& item) {
        check(item.is_interleaved && played, "media preceded PLAY response");
        if (item.channel == 7 || item.channel == 9) {
            for (const auto& report : rtsp::parse_rtcp_compound(item.payload)) {
                if (report.type == 203) {
                    ++byes;
                } // RTCP BYE
            }
            return;
        }
        if (item.channel == 8) {
            const auto rtp = rtsp::parse_rtp_packet(item.payload);
            check(units > 0 && rtp.payload_type == 97 && rtp.sequence_number == audio_sequence_++
                      && rtp.marker && rtp.payload.size() > 4, "live AAC RTP mismatch or arrived before IDR");
            check(!audio_units || rtp.timestamp > audio_timestamp_, "live AAC timestamps stopped advancing");
            audio_timestamp_ = rtp.timestamp;
            ++audio_units;
            return;
        }
        check(item.channel == 6, "wrong camera RTP channel");
        const auto rtp = rtsp::parse_rtp_packet(item.payload);
        check(rtp.payload_type == 96 && rtp.sequence_number == sequence_++, "camera RTP sequence/PT mismatch");
        if (!ssrc) {
            ssrc = rtp.ssrc;
        }
        check(ssrc == rtp.ssrc, "camera SSRC changed");
        if (!timestamp_) {
            timestamp_ = rtp.timestamp;
        }
        check(*timestamp_ == rtp.timestamp, "timestamp changed inside AU");
        fragmented = fragmented || ((static_cast<unsigned char>(rtp.payload[0]) & 31) == 28);
        std::vector<std::string> nalus;
        check(depacketizer_.push(rtp, nalus) != rtsp::H264Dropped, "camera FU-A assembly failed");
        for (const auto& nalu : nalus) {
            const auto type = static_cast<unsigned char>(nalu[0]) & 31;
            if (type == 7) {
                sps_ = nalu;
            }
            if (type == 8) {
                pps_ = nalu;
            }
            if (type == 5) {
                idr_ = true;
            }
            au_.append("\0\0\0\1", 4);
            au_ += nalu;
        }
        if (!rtp.marker) {
            return;
        }
        depacketizer_.finish();
        if (!units) {
            check(idr_ && !sps_.empty() && !pps_.empty() && rtp.timestamp == 0, "first camera AU was not IDR/SPS/PPS");
            check(rtsp::make_h264_fmtp(sps_, pps_) == fmtp, "SDP differs from on-wire SPS/PPS");
        } else {
            check(rtp.timestamp > timestamps.back(), "camera RTP time did not advance");
        }
        timestamps.push_back(rtp.timestamp);
        check(av_new_packet(packet_, static_cast<int>(au_.size())) >= 0, "allocate network AU failed");
        std::copy(au_.begin(), au_.end(), packet_->data);
        check(avcodec_send_packet(decoder_, packet_) >= 0, "submit network AU failed");
        av_packet_unref(packet_);
        decode_frames();
        au_.clear();
        timestamp_.reset();
        idr_ = false;
        ++units;
    }
    rtsp::RtspStreamParser parser_;
    rtsp::H264Depacketizer depacketizer_;
    AVCodecContext* decoder_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    int width_, height_;
    int cseq_ = 0;
    std::uint16_t sequence_ = 100;
    std::uint16_t audio_sequence_ = 100;
    std::uint32_t audio_timestamp_ = 0;
    std::optional<std::uint32_t> timestamp_;
    bool idr_ = false;
    std::string session_, au_, sps_, pps_;
};

void drain(Peer& a, Peer* b = nullptr) {
    const auto deadline = Clock::now() + 4s;
    while (!a.eof || (b && !b->eof)) {
        check(Clock::now() < deadline, "camera connection failed to close");
        a.pump();
        if (b) {
            b->pump();
        }
    }
}

void test_shared_live_stream() {
    const auto state = std::make_shared<InputState>();
    Server server(state);
    const auto port = server.server.local_port();
    Peer first(port);
    first.setup();
    first.play();
    first.wait_units(1, 1);
    std::this_thread::sleep_for(100ms);
    first.keepalive();
    check(first.units == 1 && !first.eof, "temporary camera wait ended the stream");
    state->allow(4);
    first.wait_units(4);
    Peer late(port);
    late.setup();
    late.play();
    state->allow(6);
    first.wait_units(6);
    late.pump(10);
    check(late.units == 0 && late.audio_units == 0, "late client started before the next IDR");
    state->allow(15);
    first.wait_units(15);
    late.wait_units(9, 1);
    check(first.ssrc != late.ssrc, "clients shared RTP state");
    check(std::equal(late.pictures.begin(), late.pictures.end(), first.pictures.begin() + 6),
          "late client decoded different pictures");

    late.teardown();
    drain(late);
    late.finish();
    check(late.byes == 2, "TEARDOWN did not send BYE for both tracks");
    state->allow(18);
    first.wait_units(18);
    server.server.stop();
    drain(first);
    first.finish();
    check(first.byes == 2, "stop did not send BYE for both tracks");
    server.stop();
    check(state->closed.load(), "camera worker did not close");
}

void test_stop_and_failure(bool failure) {
    const auto state = std::make_shared<InputState>();
    Server server(state);
    Peer peer(server.server.local_port());
    peer.setup();
    peer.play();
    peer.wait_units(1, 1);
    if (failure) {
        state->allow(1, false, true);
    } else {
        server.server.stop();
    }
    drain(peer);
    if (!failure) {
        peer.finish();
        check(peer.byes == 2, "server stop lost final BYE");
    }
    await([&] {
        return state->closed.load();
    });
    if (failure) {
        Peer ended(server.server.local_port());
        ended.setup();
        ended.play(455);
    }
    server.stop();
    server.stop();
    check(server.server.connection_count() == 0, "camera shutdown leaked connections");
}

void test_bind_failure() {
    rtsp::net::TcpSocket occupied;
    occupied.listen("127.0.0.1", 0);
    const auto state = std::make_shared<InputState>();
    const rtsp::CaptureTimeline timeline;
    auto video = video_source(state, false, timeline);
    rtsp::RtspServer server("127.0.0.1", occupied.local_port(),
                            std::move(video), audio_source(timeline), timeline);
    bool rejected = false;
    try {
        server.run();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected && state->closed.load(), "bind failure leaked camera worker");
}

void test_dual_live() {
    const auto state = std::make_shared<InputState>();
    Server server(state, true);
    const auto port = server.server.local_port();

    Peer slow(port, true, 640, 360);
    slow.setup();
    slow.play();
    state->allow(600);
    await([&] { return slow.socket.wait_readable(0); });
    Peer late(port, false, 640, 360);
    late.setup();
    late.play();
    late.wait_units(2);

    const auto deadline = Clock::now() + 12s;
    while (server.server.connection_count() != 1) {
        check(Clock::now() < deadline && !late.eof, "dual-track slow client was not isolated");
        late.pump(10);
    }
    const int previous_audio = late.audio_units;
    late.wait_units(late.units + 8);
    check(late.audio_units > previous_audio && late.fragmented, "surviving peer lost a track");
    late.socket.close();
    await([&] { return server.server.connection_count() == 0; });

    const int before_idle = state->produced.load();
    await([&] { return state->produced.load() >= before_idle + 12; });
    Peer resumed(port, false, 640, 360);
    resumed.setup();
    resumed.play();
    resumed.wait_units(8);
    check(resumed.audio_units > 0, "idle resume lost AAC");

    resumed.socket.close();
    server.stop();
    check(state->closed.load(), "dual-track shutdown leaked the camera input");
}
}

int main(int argc, char* argv[]) {
    try {
        // 仅测试程序提供这个入口，供独立 FFmpeg/ffplay 做无设备网络验收。
        if (argc == 2 && std::string(argv[1]) == "--serve") {
            const auto state = std::make_shared<InputState>();
            Server server(state);
            state->allow(100000);
            std::cout << "live_test_url=rtsp://127.0.0.1:" << server.server.local_port() << "/live" << std::endl;
            const auto until = Clock::now() + 12s;
            while (Clock::now() < until) {
                std::this_thread::sleep_for(20ms);
            }
            server.stop();
            return 0;
        }
        check(argc == 1, "unknown camera test argument");
        test_shared_live_stream();
        test_stop_and_failure(false);
        test_stop_and_failure(true);
        test_bind_failure();
        test_dual_live();
        std::cout << "PASS: camera + microphone RTSP/TCP, late join, slow client, resume, failure and stop\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
