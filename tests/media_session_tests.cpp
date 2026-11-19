#include "event_test_driver.h"
#include "media/file_video_source.h"
#include "media/file_audio_source.h"
#include "media/media_session.h"
#include "rtsp/rtsp_server_connection.h"
#include "rtsp/rtsp_stream_parser.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
const std::string input_path = "media_session_test.h264";
const std::string audio_input_path = "media_session_test.aac";

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
void wait_for(Predicate ready) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    for (;;) {
        test::pump();
        if (ready()) {
            break;
        }
        check(Clock::now() < deadline, "timed out waiting for media");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void save_sample(int count, int idr_interval) {
    std::ofstream output(input_path, std::ios::binary);
    const std::string prefix("\0\0\0\1", 4);
    for (int index = 0; index < count; ++index) {
        output << prefix << std::string("\x09\xf0", 2);
        if (index == 0) {
            output << prefix << std::string("\x67\x64\0\x1f", 4);
            output << prefix << std::string("\x68\xee", 2);
        }
        output << prefix << static_cast<char>(index % idr_interval == 0 ? 0x65 : 0x41)
               << static_cast<char>(0x30 + index);
    }
    check(static_cast<bool>(output), "failed to save sample");
}

struct Peer {
    explicit Peer(const rtsp::MediaTrack& track) : Peer(1024 * 1024, 0, 100, 1, track) {}
    explicit Peer(std::size_t limit = 1024 * 1024, int channel = 0,
                  std::uint16_t sequence = 100, std::uint32_t ssrc = 1,
                  const rtsp::MediaTrack& track = rtsp::MediaTrack())
        : queue(std::make_shared<rtsp::net::OutboundQueue>(limit)) {
        rtsp::TcpTransport tcp;
        tcp.rtp_channel = channel;
        tcp.rtcp_channel = channel + 1;
        subscription = std::make_shared<rtsp::MediaSubscription>(queue);
        subscription->add_track(0, track, tcp, sequence, ssrc);
    }
    std::shared_ptr<rtsp::net::OutboundQueue> queue;
    std::shared_ptr<rtsp::MediaSubscription> subscription;
};

rtsp::net::OutboundMessage pop(Peer& peer) {
    wait_for([&] {
        return peer.queue->bytes() != 0 || peer.queue->closed();
    });
    rtsp::net::OutboundMessage message;
    check(peer.queue->try_pop(message), "unexpected end of queue");
    return message;
}

void play(test::DrivenMedia& media, Peer& peer) {
    check(media.subscribe(peer.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
          "subscription was rejected");
    const auto response = pop(peer);
    check(response.kind == rtsp::net::OutboundKind::RtspResponse && response.wire == "PLAY 200",
          "RTP preceded PLAY response");
}

struct Frame {
    int index = -1;
    std::vector<unsigned int> types;
    std::uint32_t timestamp = 0;
};

Frame read_frame(Peer& peer, std::uint16_t& sequence, int channel, std::uint32_t ssrc) {
    Frame frame;
    for (;;) {
        const auto message = pop(peer);
        rtsp::RtspStreamParser parser;
        parser.append(message.wire.data(), message.wire.size());
        rtsp::RtspStreamItem item;
        check(parser.next(item) && item.is_interleaved
                  && item.channel == static_cast<unsigned int>(channel), "wrong RTP channel");
        parser.finish();
        const auto packet = rtsp::parse_rtp_packet(item.payload);
        check(packet.sequence_number == sequence++ && packet.ssrc == ssrc,
              "RTP state was shared between clients");
        if (!frame.types.empty()) {
            check(packet.timestamp == frame.timestamp, "timestamp changed within one AU");
        }
        frame.timestamp = packet.timestamp;
        const unsigned int type = static_cast<unsigned char>(packet.payload[0]) & 0x1f;
        frame.types.push_back(type);
        if (type == 1 || type == 5) {
            frame.index = static_cast<unsigned char>(packet.payload[1]) - 0x30;
        }
        if (packet.marker) {
            check(frame.index >= 0, "marker appeared before picture");
            return frame;
        }
    }
}

void test_shared_progress_and_late_join() {
    save_sample(12, 5);
    test::DrivenMedia media(input_path);
    // 后续订阅仍能成功，证明它们没有重新打开文件。
    check(std::remove(input_path.c_str()) == 0, "input remained open after loading");
    Peer a;
    play(media, a);
    std::uint16_t sequence_a = 100;
    check(read_frame(a, sequence_a, 0, 1).index == 0, "first client missed start");
    check(read_frame(a, sequence_a, 0, 1).index == 1, "first client skipped AU");

    Peer b(1024 * 1024, 6, 800, 2);
    play(media, b);
    std::uint16_t sequence_b = 800;
    const auto first = read_frame(b, sequence_b, 6, 2);
    check(first.index == 5 && first.timestamp == 0, "late join did not wait for next IDR");
    check(first.types == std::vector<unsigned int>({9, 7, 8, 5}), "late IDR missed SPS/PPS");
    for (int index = 2; index < 12; ++index) {
        const auto frame = read_frame(a, sequence_a, 0, 1);
        check(frame.index == index && frame.timestamp == static_cast<std::uint32_t>(index * 3600),
              "first client progress changed");
    }
    for (int index = 6; index < 12; ++index) {
        const auto frame = read_frame(b, sequence_b, 6, 2);
        check(frame.index == index
                  && frame.timestamp == static_cast<std::uint32_t>((index - 5) * 3600),
              "clients did not share remaining AU sequence");
    }
    wait_for([&] {
        return media.ended() && a.queue->closed() && b.queue->closed();
    });
    check(!a.subscription->failed() && !b.subscription->failed(), "normal EOF reported failure");
    Peer after_eof;
    check(media.subscribe(after_eof.subscription, "PLAY 200") == rtsp::SubscribeResult::Ended
              && after_eof.queue->bytes() == 0, "EOF resource restarted or emitted 200");
}

void test_pause_cancel_and_stop() {
    save_sample(10, 1);
    test::DrivenMedia media(input_path);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Peer a;
    play(media, a);
    std::uint16_t seq_a = 100;
    check(read_frame(a, seq_a, 0, 1).index == 0, "source advanced before PLAY");
    a.subscription->cancel();
    const auto bytes = a.queue->bytes();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    check(a.queue->bytes() == bytes && !media.ended(), "cancelled subscription still advanced media");

    Peer b;
    check(media.subscribe(b.subscription, "PLAY 200", true) == rtsp::SubscribeResult::InvalidRange
              && b.queue->bytes() == 0, "shared progress unexpectedly rewound");
    const auto resumed = Clock::now();
    play(media, b);  // 被拒绝的 Range 不能影响后续不带 Range 的 PLAY。
    std::uint16_t seq_b = 100;
    check(read_frame(b, seq_b, 0, 1).index == 1, "idle period consumed file or restarted it");
    check(read_frame(b, seq_b, 0, 1).index == 2, "resume skipped AU");
    check(read_frame(b, seq_b, 0, 1).index == 3, "resume skipped AU");
    check(Clock::now() - resumed >= std::chrono::milliseconds(60), "resume burst old frames");
    media.stop();
    check(media.ended() && b.subscription->finished() && b.queue->closed(), "stop leaked subscription");
    // 已取消订阅的队列仍属于连接，资源结束不能抢先关闭 TEARDOWN 输出。
    check(a.queue->push({rtsp::net::OutboundKind::RtspResponse, "TEARDOWN 200"}),
          "resource closed a cancelled subscription queue");
}

void test_slow_peer_and_waiting_at_eof() {
    save_sample(8, 3);
    test::DrivenMedia media(input_path);
    Peer healthy;
    play(media, healthy);
    std::uint16_t seq = 100;
    check(read_frame(healthy, seq, 0, 1).index == 0, "healthy client missed start");
    Peer slow(32);
    play(media, slow);
    wait_for([&] {
        return slow.subscription->failed();
    });
    for (int index = 1; index < 8; ++index) {
        check(read_frame(healthy, seq, 0, 1).index == index, "slow peer affected healthy client");
    }
    wait_for([&] {
        return media.ended();
    });
    check(!healthy.subscription->failed(), "slow peer ended shared source");

    save_sample(4, 10);  // 只有开头一个 IDR，晚加入者一直等到 EOF。
    test::DrivenMedia no_more_idr(input_path);
    Peer a;
    play(no_more_idr, a);
    seq = 100;
    read_frame(a, seq, 0, 1);
    Peer waiting;
    play(no_more_idr, waiting);
    wait_for([&] {
        return waiting.queue->closed();
    });
    check(waiting.queue->bytes() == 0 && waiting.subscription->finished(),
          "waiting peer received P frames or remained subscribed at EOF");
}

void test_failed_admission_and_idle_stop() {
    save_sample(4, 1);
    test::DrivenMedia media(input_path);
    Peer closed;
    closed.queue->close();
    check(media.subscribe(closed.subscription, "PLAY 200") == rtsp::SubscribeResult::QueueClosed,
          "closed queue accepted subscription");
    Peer valid;
    check(media.subscribe(valid.subscription, "PLAY 200", true) == rtsp::SubscribeResult::Started,
          "failed admission advanced source");
    media.stop();
    check(valid.queue->closed(), "stopping immediately after PLAY leaked subscription");

    test::DrivenMedia idle(input_path);
    idle.stop();
    check(idle.ended(), "idle scheduler did not wake on stop");
}

void test_injected_file_source() {
    save_sample(3, 2);
    rtsp::MediaSession by_path(input_path);
    auto source = std::make_unique<rtsp::FileVideoSource>(input_path);
    const auto expected_sdp = rtsp::make_sdp({"*", {source->track()}});
    rtsp::MediaSession injected(std::move(source));
    check(rtsp::make_sdp(injected.description()) == expected_sdp
              && rtsp::make_sdp(by_path.description()) == expected_sdp,
          "injected source changed SDP");
    check(std::remove(input_path.c_str()) == 0, "session reopened file after construction");
    Peer a;
    Peer b;
    check(by_path.subscribe(a.subscription, "PLAY 200") == rtsp::SubscribeResult::Started
              && injected.subscribe(b.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
          "injected file subscription failed");
    const auto start = Clock::now();
    std::size_t messages = 0;
    for (int index = 0; index < 3; ++index) {
        const auto now = start + std::chrono::milliseconds(index * 40);
        by_path.tick(now);
        injected.tick(now);
        check(by_path.next_deadline() == injected.next_deadline()
                  && by_path.ended() == injected.ended(), "injected source changed schedule or EOF");
        rtsp::net::OutboundMessage left;
        rtsp::net::OutboundMessage right;
        while (a.queue->try_pop(left)) {
            check(b.queue->try_pop(right) && left.kind == right.kind && left.wire == right.wire,
                  "injected source changed RTSP/RTP/RTCP output");
            ++messages;
        }
        check(!b.queue->try_pop(right), "injected source emitted extra output");
    }
    check(messages > 3 && by_path.ended() && injected.ended()
              && a.queue->closed() && b.queue->closed()
              && !a.subscription->failed() && !b.subscription->failed(),
          "injected file source did not finish normally");
}

class WaitingSource final : public rtsp::VideoSource {
public:
    bool read(rtsp::H264AccessUnit& frame) override {
        frame = {};
        ++reads;
        if (fail) {
            throw std::runtime_error("simulated live input failure");
        }
        if (!ready) {
            return false;
        }
        ready = false;
        frame = {{std::string("\x65\x30", 2)}, true, pts, 3000};
        return true;
    }
    bool ended() const override {
        return eof;
    }
    const rtsp::MediaTrack& track() const override {
        return track_;
    }
    bool is_live() const override {
        return true;
    }
    void resume() override {
        ++resumes;
    }
    void stop() override {
        stopped = true;
    }
    bool ready = false;
    bool eof = false;
    bool fail = false;
    bool stopped = false;
    std::int64_t pts = 0;
    int reads = 0;
    int resumes = 0;
private:
    rtsp::MediaTrack track_;
};

void test_live_waiting_and_end() {
    for (bool failure : {false, true}) {
        auto input = std::make_unique<WaitingSource>();
        auto* source = input.get();
        rtsp::MediaSession media(std::move(input));
        const auto start = Clock::now();
        media.tick(start);
        check(source->reads == 0 && !media.next_deadline(), "idle live source advanced");
        Peer peer;
        check(media.subscribe(peer.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
              "live subscription rejected");
        media.tick(start);
        check(!media.ended() && !peer.queue->closed() && !peer.subscription->finished()
                  && media.next_deadline() == start + std::chrono::milliseconds(5),
              "Waiting caused EOF or busy polling");
        source->ready = true;
        media.tick(start + std::chrono::milliseconds(5));
        check(!media.ended() && !peer.queue->closed(), "frame followed by Waiting ended live source");
        if (failure) {
            source->fail = true;
        } else {
            source->eof = true;
        }
        media.tick(start + std::chrono::milliseconds(10));
        check(media.ended() && source->stopped && peer.queue->closed()
                  && peer.subscription->failed() == failure,
              "live EOF/failure did not stop source and subscription");
        bool saw_rtp = false;
        bool saw_final = false;
        rtsp::net::OutboundMessage message;
        while (peer.queue->try_pop(message)) {
            saw_rtp = saw_rtp || message.kind == rtsp::net::OutboundKind::Rtp;
            saw_final = saw_final || message.final_rtcp;
        }
        check(saw_rtp && saw_final == !failure, "live waiting or failure corrupted final RTCP");
    }
}

void test_live_wait_with_future_audio() {
    const std::string audio_path = "media_session_live_wait.aac";
    {
        std::ofstream output(audio_path, std::ios::binary);
        const std::string adts("\xff\xf1\x4c\x80\x01\x5f\xfc\x11\x22\x33", 10);
        output << adts << adts;
        check(static_cast<bool>(output), "failed to save live-wait audio");
    }
    auto input = std::make_unique<WaitingSource>();
    auto* source = input.get();
    rtsp::MediaSession media(std::move(input), audio_path);
    check(std::remove(audio_path.c_str()) == 0, "audio input stayed open");
    Peer peer;
    peer.subscription->add_track(1, media.description().tracks[1], {2, 3}, 200, 2);
    check(media.subscribe(peer.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
          "live audio/video PLAY failed");
    const auto now = Clock::now();
    media.tick(now);
    check(media.next_deadline() == now + std::chrono::milliseconds(5) && !media.ended(),
          "future audio suppressed short video retry");
    source->ready = true;
    media.tick(now + std::chrono::milliseconds(5));
    check(media.next_deadline() == now + std::chrono::milliseconds(10),
          "video retry was lost after IDR");
    media.tick(now + std::chrono::milliseconds(30));
    check(!media.ended(), "audio EOF ended waiting live video");
    source->eof = true;
    media.tick(now + std::chrono::milliseconds(40));
    check(media.ended() && !peer.subscription->failed(), "live video EOF failed after audio drained");
}

void test_injected_audio_file(bool with_video) {
    save_sample(3, 2);
    {
        std::ofstream output(audio_input_path, std::ios::binary);
        const std::string adts("\xff\xf1\x4c\x80\x01\x5f\xfc\x11\x22\x33", 10);
        output << adts << adts << adts;
        check(static_cast<bool>(output), "failed to save injected audio fixture");
    }
    auto video = [with_video]() -> std::unique_ptr<rtsp::VideoSource> {
        return with_video ? std::make_unique<rtsp::FileVideoSource>(input_path) : nullptr;
    };
    rtsp::MediaSession paths(with_video ? input_path : "", audio_input_path);
    rtsp::MediaSession video_and_path(video(), audio_input_path);
    rtsp::MediaSession sources(video(), std::make_unique<rtsp::FileAudioSource>(audio_input_path));
    check(rtsp::make_sdp(paths.description()) == rtsp::make_sdp(sources.description())
              && rtsp::make_sdp(video_and_path.description()) == rtsp::make_sdp(sources.description()),
          "audio source injection changed SDP or legacy constructors");
    check(std::remove(audio_input_path.c_str()) == 0, "injected audio file stayed open");
    const auto& tracks = sources.description().tracks;
    check(tracks.size() == (with_video ? 2u : 1u) && tracks.back().control == "trackID=1",
          "audio source injection changed track order or control");
    Peer a(tracks[0]);
    Peer b(tracks[0]);
    Peer c(tracks[0]);
    for (auto* peer : {&a, &b, &c}) {
        if (with_video) { peer->subscription->add_track(1, tracks[1], {2, 3}, 200, 2); }
    }
    check(paths.subscribe(a.subscription, "PLAY 200") == rtsp::SubscribeResult::Started
              && video_and_path.subscribe(b.subscription, "PLAY 200") == rtsp::SubscribeResult::Started
              && sources.subscribe(c.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
          "injected audio subscription failed");
    const auto now = Clock::now();
    for (int elapsed = 0; elapsed <= 100; elapsed += 5) {
        const auto time = now + std::chrono::milliseconds(elapsed);
        paths.tick(time);
        video_and_path.tick(time);
        sources.tick(time);
    }
    check(paths.ended() && video_and_path.ended() && sources.ended()
              && !a.subscription->failed() && !b.subscription->failed() && !c.subscription->failed(),
          "injected audio did not drain normally");
    int audio_packets = 0;
    rtsp::net::OutboundMessage first;
    while (a.queue->try_pop(first)) {
        rtsp::net::OutboundMessage second;
        rtsp::net::OutboundMessage third;
        check(b.queue->try_pop(second) && c.queue->try_pop(third)
                  && first.kind == second.kind && first.kind == third.kind
                  && first.wire == second.wire && first.wire == third.wire,
              "audio source injection changed output");
        if (first.kind != rtsp::net::OutboundKind::Rtp) {
            continue;
        }
        rtsp::RtspStreamParser parser;
        parser.append(first.wire.data(), first.wire.size());
        rtsp::RtspStreamItem item;
        check(parser.next(item) && item.is_interleaved, "invalid injected RTP");
        if (item.channel != (with_video ? 2u : 0u)) {
            continue;
        }
        const auto packet = rtsp::parse_rtp_packet(item.payload);
        check(packet.payload_type == 97 && packet.timestamp == audio_packets * 1024u
                  && packet.payload == std::string("\0\x10\0\x18\x11\x22\x33", 7),
              "injected audio changed RTP mapping, timestamp or payload");
        ++audio_packets;
    }
    check(audio_packets == 3 && b.queue->bytes() == 0 && c.queue->bytes() == 0,
          "audio source injection lost or added packets");
}

class WaitingAudioSource final : public rtsp::AudioSource {
public:
    WaitingAudioSource() {
        track_.media_type = "audio";
        track_.control = "trackID=1";
        track_.payload_type = 97;
        track_.encoding = "MPEG4-GENERIC";
        track_.clock_rate = 48000;
        track_.channels = 2;
        track_.fmtp = "streamtype=5;profile-level-id=1;mode=AAC-hbr;config=1190;"
                      "SizeLength=13;IndexLength=3;IndexDeltaLength=3";
    }
    bool read(rtsp::AacAccessUnit& frame) override {
        frame = {};
        ++reads;
        if (fail) {
            throw std::runtime_error("simulated live audio failure");
        }
        if (!ready) {
            return false;
        }
        ready = false;
        frame = {std::string("\x11\x22\x33", 3), pts, 1024};
        return true;
    }
    bool ended() const override {
        return eof;
    }
    const rtsp::MediaTrack& track() const override {
        return track_;
    }
    bool is_live() const override {
        return true;
    }
    void resume() override {
        ++resumes;
    }
    void stop() override {
        ++stops;
    }
    bool ready = false;
    bool eof = false;
    bool fail = false;
    std::int64_t pts = 0;
    int reads = 0;
    int resumes = 0;
    int stops = 0;
private:
    rtsp::MediaTrack track_;
};

void test_capture_timeline() {
    using namespace std::chrono_literals;
    const auto epoch = Clock::now();
    const auto timeline = std::make_shared<const rtsp::CaptureTimeline>(epoch);
    auto video = std::make_unique<WaitingSource>();
    auto audio = std::make_unique<WaitingAudioSource>();
    auto* video_input = video.get();
    video->ready = audio->ready = true;
    video->pts = 7200;  // 80 ms at 90 kHz
    audio->pts = 9600;  // 200 ms at 48 kHz
    rtsp::MediaSession media(std::move(video), std::move(audio), timeline);
    auto queue = std::make_shared<rtsp::net::OutboundQueue>();
    auto subscription = std::make_shared<rtsp::MediaSubscription>(queue);
    const auto& tracks = media.description().tracks;
    constexpr std::uint32_t video_base = 0xffffff00, audio_base = 0xfffffff0;
    subscription->add_track(0, tracks[0], {0, 1}, 100, 1, video_base);
    subscription->add_track(1, tracks[1], {2, 3}, 200, 2, audio_base);
    check(media.subscribe(subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
          "capture timeline subscription failed");
    rtsp::net::OutboundMessage message;
    check(queue->try_pop(message) && message.kind == rtsp::net::OutboundKind::RtspResponse,
          "missing PLAY response");

    const auto receive_rtp = [&](unsigned int channel) {
        check(queue->try_pop(message) && message.kind == rtsp::net::OutboundKind::Rtp,
              "missing timeline RTP");
        rtsp::RtspStreamParser parser;
        parser.append(message.wire.data(), message.wire.size());
        rtsp::RtspStreamItem item;
        check(parser.next(item) && item.is_interleaved && item.channel == channel, "wrong RTP channel");
        subscription->on_sent(message);
        return rtsp::parse_rtp_packet(item.payload).timestamp;
    };
    media.tick(epoch + 229ms);
    check(!queue->try_pop(message) && media.next_deadline() == epoch + 230ms,
          "video did not wait until T0 + PTS + send buffer");
    media.tick(epoch + 230ms);
    const auto video_rtp = receive_rtp(0);
    media.tick(epoch + 349ms);
    check(!queue->try_pop(message) && media.next_deadline() == epoch + 350ms,
          "audio did not wait until T0 + PTS + send buffer");
    media.tick(epoch + 350ms);
    const auto audio_rtp = receive_rtp(2);
    check(video_rtp == video_base && audio_rtp == std::uint32_t(audio_base + 5760u),
          "RTP lost the common subscription offset or did not wrap");

    const auto report_at = epoch + 400ms;
    subscription->tick_rtcp(report_at, 1s);
    const auto sample_time = [&](Clock::time_point now, std::uint32_t ssrc,
                                 std::uint32_t timestamp, int clock_rate) {
        check(queue->try_pop(message) && message.kind == rtsp::net::OutboundKind::Rtcp,
              "missing sender report");
        const auto wire = subscription->prepare_rtcp(message, media.clock(), now);
        rtsp::RtspStreamParser parser;
        parser.append(wire.data(), wire.size());
        rtsp::RtspStreamItem item;
        check(parser.next(item) && item.is_interleaved, "invalid interleaved RTCP");
        const auto packets = rtsp::parse_rtcp_compound(item.payload);
        check(!packets.empty() && packets.front().type == 200, "expected RTCP SR");
        const auto& sr = packets.front();
        check(sr.ssrc == ssrc && sr.ntp == media.clock().ntp_at(now) && sr.packet_count == 1,
              "wrong SR stream, report time or count");
        const auto elapsed = static_cast<std::uint32_t>(sr.rtp_timestamp - timestamp);
        const auto ntp_since_epoch = static_cast<std::int64_t>(sr.ntp - media.clock().ntp_at(epoch));
        return ntp_since_epoch / 4294967296.0 - static_cast<double>(elapsed) / clock_rate;
    };
    const auto video_seconds = sample_time(report_at, 1, video_rtp, 90000);
    const auto audio_seconds = sample_time(report_at + 37ms, 2, audio_rtp, 48000);
    const double tolerance = 1.0 / 48000;
    check(std::abs(video_seconds - 0.080) < tolerance && std::abs(audio_seconds - 0.200) < tolerance &&
          std::abs(audio_seconds - video_seconds - 0.120) < tolerance,
          "SR did not preserve absolute capture time and the 120 ms offset");

    subscription->cancel();
    media.tick(epoch + 450ms);
    video_input->ready = true;
    video_input->pts = 45000;  // T0 + 500 ms, send at T0 + 650 ms
    Peer resumed(tracks[0]);
    check(media.subscribe(resumed.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
          "capture timeline resubscribe failed");
    media.tick(epoch + 640ms);
    check(resumed.queue->try_pop(message) && message.kind == rtsp::net::OutboundKind::RtspResponse &&
          !resumed.queue->try_pop(message), "capture epoch was reset on resubscribe");
    media.tick(epoch + 650ms);
    check(resumed.queue->try_pop(message) && message.kind == rtsp::net::OutboundKind::Rtp,
          "resumed peer did not receive video at the original deadline");
}

void test_audio_waiting_and_end() {
    for (bool failure : {false, true}) {
        auto input = std::make_unique<WaitingAudioSource>();
        auto* source = input.get();
        rtsp::MediaSession media(nullptr, std::move(input));
        const auto now = Clock::now();
        media.tick(now);
        check(source->reads == 0 && !media.next_deadline(), "idle audio source advanced");
        Peer peer(source->track());
        check(media.subscribe(peer.subscription, "PLAY 200") == rtsp::SubscribeResult::Started
                  && source->resumes == 1, "live audio was not resumed");
        media.tick(now);
        check(!media.ended() && !peer.queue->closed()
                  && media.next_deadline() == now + std::chrono::milliseconds(5),
              "audio Waiting caused EOF or busy polling");
        source->ready = true;
        media.tick(now + std::chrono::milliseconds(5));
        check(!media.ended() && !peer.queue->closed(), "audio Waiting after AU caused EOF");
        source->fail = failure;
        source->eof = !failure;
        media.tick(now + std::chrono::milliseconds(10));
        media.stop();
        check(media.ended() && source->stops == 1 && peer.queue->closed()
                  && peer.subscription->failed() == failure,
              "audio EOF/failure did not stop source and subscription");
        bool saw_rtp = false;
        bool saw_final = false;
        rtsp::net::OutboundMessage message;
        while (peer.queue->try_pop(message)) {
            saw_rtp = saw_rtp || message.kind == rtsp::net::OutboundKind::Rtp;
            saw_final = saw_final || message.final_rtcp;
        }
        check(saw_rtp && saw_final == !failure, "audio failure corrupted final RTCP");
    }
}

void test_audio_resume_and_stop() {
    auto input = std::make_unique<WaitingAudioSource>();
    auto* source = input.get();
    rtsp::MediaSession media(nullptr, std::move(input));
    Peer a(source->track());
    Peer b(source->track());
    Peer c(source->track());
    check(media.subscribe(a.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
          "first audio subscriber failed");
    source->ready = true;
    const auto now = Clock::now();
    media.tick(now);
    source->ready = true;
    source->pts = 48000;
    media.tick(now);
    const int reads = source->reads;
    check(media.subscribe(b.subscription, "PLAY 200") == rtsp::SubscribeResult::Started
              && source->resumes == 1, "late subscriber resumed shared audio");
    media.tick(now);
    check(source->reads == reads && media.next_deadline() == now + std::chrono::seconds(1),
          "late subscriber discarded prefetched audio");
    a.subscription->cancel();
    b.subscription->cancel();
    check(media.subscribe(c.subscription, "PLAY 200") == rtsp::SubscribeResult::Started
              && source->resumes == 2, "audio did not resume after idle");
    source->eof = true;
    media.tick(now);
    check(media.ended() && source->stops == 1, "audio resume retained stale prefetched AU");

    auto idle = std::make_unique<WaitingAudioSource>();
    auto* idle_source = idle.get();
    rtsp::MediaSession stopped(nullptr, std::move(idle));
    stopped.stop();
    stopped.stop();
    check(stopped.ended() && idle_source->stops == 1 && idle_source->reads == 0,
          "explicit stop did not release idle audio exactly once");
}

void test_audio_wait_with_future_video() {
    save_sample(3, 2);
    auto input = std::make_unique<WaitingAudioSource>();
    auto* source = input.get();
    rtsp::MediaSession media(std::make_unique<rtsp::FileVideoSource>(input_path), std::move(input));
    Peer peer(media.description().tracks[0]);
    peer.subscription->add_track(1, media.description().tracks[1], {2, 3}, 200, 2);
    check(media.subscribe(peer.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
          "video/audio source subscription failed");
    const auto now = Clock::now();
    media.tick(now);
    check(media.next_deadline() == now + std::chrono::milliseconds(5),
          "future video suppressed audio retry");
    source->ready = true;
    source->pts = 240;
    media.tick(now + std::chrono::milliseconds(5));
    bool saw_audio = false;
    rtsp::net::OutboundMessage message;
    while (peer.queue->try_pop(message)) {
        if (message.kind != rtsp::net::OutboundKind::Rtp) {
            continue;
        }
        rtsp::RtspStreamParser parser;
        parser.append(message.wire.data(), message.wire.size());
        rtsp::RtspStreamItem item;
        check(parser.next(item), "invalid live audio/video RTP");
        saw_audio = saw_audio || item.channel == 2;
    }
    check(saw_audio && media.next_deadline() == now + std::chrono::milliseconds(10),
          "audio did not send before future video");
    source->eof = true;
    media.tick(now + std::chrono::milliseconds(10));
    check(!media.ended() && media.next_deadline() == now + std::chrono::milliseconds(40),
          "audio EOF stopped video or retained retry");
    media.tick(now + std::chrono::milliseconds(100));
    check(media.ended() && source->stops == 1 && !peer.subscription->failed(),
          "video did not drain after audio EOF");
}

void test_empty_audio_video_sources() {
    bool rejected = false;
    try {
        rtsp::MediaSession media(std::unique_ptr<rtsp::VideoSource>{},
                                std::unique_ptr<rtsp::AudioSource>{});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "session accepted two empty sources");
}

// 同一个测试线程驱动收发，验证控制阶段和播放阶段都能安全关闭。
void test_connection_stop(bool playing) {
    save_sample(20, 5);
    auto media = std::make_shared<test::DrivenMedia>(input_path);
    rtsp::net::TcpSocket listener;
    listener.listen("127.0.0.1", 0);
    rtsp::net::TcpSocket client;
    client.connect("127.0.0.1", listener.local_port());
    test::DrivenConnection connection(media);
    connection.accept_from(listener);


    if (playing) {
        rtsp::RtspStreamParser parser;
        auto response = [&] {
            rtsp::RtspStreamItem item;
            char buffer[4096];
            const auto deadline = Clock::now() + std::chrono::seconds(3);
            while (!parser.next(item)) {
                check(Clock::now() < deadline, "connection response timed out");
                test::pump();
                if (client.wait_readable(5)) {
                    const auto count = client.receive(buffer, sizeof(buffer));
                    check(count != 0, "connection ended before responding");
                    parser.append(buffer, count);
                }
            }
            check(!item.is_interleaved && item.message.status_code == 200,
                  "connection sent media before response or rejected request");
        };
        const std::string setup = "SETUP rtsp://127.0.0.1/live/trackID=0 RTSP/1.0\r\n"
                                  "CSeq: 1\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";
        client.send_all(setup.data(), setup.size());
        response();
        const std::string play_request = "PLAY rtsp://127.0.0.1/live RTSP/1.0\r\n"
            "CSeq: 2\r\nSession: " + connection.session_id() + "\r\n\r\n";
        client.send_all(play_request.data(), play_request.size());
        response();
    }
    const auto start = Clock::now();
    connection.close();
    test::wait_finished(connection);
    check(connection.finished() && Clock::now() - start < std::chrono::seconds(2),
          "connection stop did not release the event-driven connection");
    media->stop();
}

}  // namespace

int main() {
    try {
        test_shared_progress_and_late_join();
        test_pause_cancel_and_stop();
        test_slow_peer_and_waiting_at_eof();
        test_failed_admission_and_idle_stop();
        test_injected_file_source();
        test_live_waiting_and_end();
        test_live_wait_with_future_audio();
        test_injected_audio_file(false);
        test_injected_audio_file(true);
        test_capture_timeline();
        test_audio_waiting_and_end();
        test_audio_resume_and_stop();
        test_audio_wait_with_future_video();
        test_empty_audio_video_sources();
        test_connection_stop(false);
        test_connection_stop(true);
        std::remove(input_path.c_str());
        std::remove(audio_input_path.c_str());
        std::cout << "PASS: shared source, late IDR, independent RTP, pause, cancellation, slow peer and EOF\n";
        return 0;
    } catch (const std::exception& error) {
        std::remove(input_path.c_str());
        std::remove(audio_input_path.c_str());
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
