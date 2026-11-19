#include "media/media_session.h"
#include "rtsp/rtsp_stream_parser.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
using Clock = rtsp::MediaSession::Clock;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Peer {
    explicit Peer(const rtsp::MediaTrack& track) {
        subscription->add_track(0, track, {0, 1}, 100, 1);
    }
    Peer() = default;
    std::shared_ptr<rtsp::net::OutboundQueue> queue = std::make_shared<rtsp::net::OutboundQueue>();
    std::shared_ptr<rtsp::MediaSubscription> subscription = std::make_shared<rtsp::MediaSubscription>(queue);
};

rtsp::net::OutboundMessage pop(Peer& peer) {
    rtsp::net::OutboundMessage message;
    check(peer.queue->try_pop(message), "unexpected empty outbound queue");
    return message;
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
        frame = {{std::string{static_cast<char>(keyframe ? 0x65 : 0x41), '0'}}, keyframe, pts, 3000};
        return true;
    }
    bool ended() const override {
        return eof;
    }
    const rtsp::MediaTrack& track() const override {
        return track_;
    }
    void check_health() const override {
        if (fail || eof) {
            throw std::runtime_error("simulated live video unavailable");
        }
    }
    void resume() override {
        ++resumes;
    }
    void stop() override {
        stopped = true;
    }
    bool ready = false;
    bool keyframe = true;
    bool eof = false;
    bool fail = false;
    bool stopped = false;
    std::int64_t pts = 0;
    int reads = 0;
    int resumes = 0;
private:
    rtsp::MediaTrack track_;
};

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
    void check_health() const override {
        if (fail || eof) {
            throw std::runtime_error("simulated live audio unavailable");
        }
    }
    void stop() override {
        ++stops;
    }
    bool ready = false;
    bool eof = false;
    bool fail = false;
    std::int64_t pts = 0;
    int reads = 0;
    int stops = 0;
private:
    rtsp::MediaTrack track_;
};

void test_capture_timeline() {
    using namespace std::chrono_literals;
    const auto epoch = Clock::now();
    const auto timeline = rtsp::CaptureTimeline(epoch);
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

void test_live_backlog_and_resume() {
    using namespace std::chrono_literals;
    const auto epoch = Clock::now();
    auto video = std::make_unique<WaitingSource>();
    auto audio = std::make_unique<WaitingAudioSource>();
    auto& v = *video;
    auto& a = *audio;
    rtsp::MediaSession media(std::move(video), std::move(audio),
        rtsp::CaptureTimeline(epoch));
    const auto& tracks = media.description().tracks;
    const auto play_both = [&](Peer& peer) {
        peer.subscription->add_track(1, tracks[1], {2, 3}, 200, 2);
        check(media.subscribe(peer.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
              "live PLAY rejected");
        check(pop(peer).kind == rtsp::net::OutboundKind::RtspResponse, "RTP preceded PLAY");
    };
    const auto feed_video = [&](int ms, bool idr) {
        v.pts = ms * 90;
        v.keyframe = idr;
        v.ready = true;
    };
    const auto feed_audio = [&](int pts) {
        a.pts = pts;
        a.ready = true;
    };
    const auto rtp = [&](Peer& peer, unsigned int channel) {
        check(!peer.queue->empty(), "missing live RTP");
        const auto message = pop(peer);
        rtsp::RtspStreamParser parser;
        parser.append(message.wire.data(), message.wire.size());
        rtsp::RtspStreamItem item;
        check(parser.next(item) && item.is_interleaved && item.channel == channel, "wrong live RTP track");
        return rtsp::parse_rtp_packet(item.payload);
    };
    const auto expect_idr = [&](Peer& peer, std::uint32_t timestamp) {
        const auto packet = rtp(peer, 0);
        check(packet.timestamp == timestamp && packet.payload[0] == 0x65, "incorrect IDR or RTP origin");
    };

    Peer first(tracks[0]), late(tracks[0]), resumed(tracks[0]);
    play_both(first);
    feed_video(0, true);
    feed_audio(0);
    media.tick(epoch + 150ms);
    expect_idr(first, 0);
    check(rtp(first, 2).timestamp == 0, "incorrect first audio timestamp");

    play_both(late);
    check(v.resumes == 1, "first subscriber did not request an IDR");
    feed_video(33, false);
    feed_audio(1024);
    media.tick(epoch + 250ms);
    check(first.queue->empty() && late.queue->empty(), "overdue RTP was replayed");

    feed_video(100, false);
    feed_audio(5120);
    media.tick(epoch + 257ms);
    check(rtp(first, 2).timestamp == 5120 && first.queue->empty() && late.queue->empty(),
          "lost the AAC gap or sent a P frame with a missing reference");

    feed_video(120, true);
    feed_audio(6144);
    media.tick(epoch + 278ms);
    expect_idr(first, 10800);
    expect_idr(late, 0);
    check(rtp(first, 2).timestamp == 6144 && rtp(late, 2).timestamp == 384,
          "late join did not preserve audio offset");

    feed_video(200, true);
    feed_audio(10240);
    media.tick(epoch + 300ms);  // Prefetch both AUs, then lose all viewers.
    first.subscription->cancel();
    late.subscription->cancel();
    media.tick(epoch + 1s);
    check(!media.next_deadline(), "idle session kept a send deadline");
    play_both(resumed);
    check(v.resumes == 2, "idle resume did not request a fresh IDR");
    feed_video(500, true);  // Even the newest queued IDR is far too old.
    feed_audio(24576);
    media.tick(epoch + 1150ms);
    check(resumed.queue->empty() && !v.ready && !a.ready, "idle resume retained or sent stale AUs");
    feed_video(1100, true);
    feed_audio(53248);
    media.tick(epoch + 1260ms);
    expect_idr(resumed, 0);
    check(rtp(resumed, 2).timestamp == 448, "idle resume lost the IDR-relative audio offset");
}

void test_required_live_source_failure() {
    for (bool active : {false, true}) {
        for (bool video_fails : {false, true}) {
            auto video = std::make_unique<WaitingSource>();
            auto audio = std::make_unique<WaitingAudioSource>();
            auto& v = *video;
            auto& a = *audio;
            const auto now = Clock::now();
            rtsp::MediaSession media(std::move(video), std::move(audio),
                rtsp::CaptureTimeline(now));
            Peer peer(media.description().tracks[0]);
            if (active) {
                check(media.subscribe(peer.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
                      "live session rejected PLAY before failure");
            }
            v.fail = video_fails;
            a.eof = !video_fails;
            media.tick(now);
            check(media.ended() && v.stopped && a.stops == 1 && v.reads == 0 && a.reads == 0,
                  "producer failure did not stop both tracks before reading more data");
            check(!active || (peer.subscription->failed() && peer.queue->closed()),
                  "active playback did not fail with the source");
            Peer later(media.description().tracks[0]);
            check(media.subscribe(later.subscription, "PLAY 200") == rtsp::SubscribeResult::Ended,
                  "unavailable stream still accepted PLAY");
        }
    }
}

void test_wait_and_validate_sources() {
    const auto t0 = Clock::now();
    const rtsp::CaptureTimeline timeline(t0);
    for (bool missing_video : {false, true}) {
        bool rejected = false;
        try {
            rtsp::MediaSession invalid(missing_video ? nullptr : std::make_unique<WaitingSource>(),
                                       missing_video ? std::make_unique<WaitingAudioSource>() : nullptr, timeline);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "accepted a missing capture source");
    }

    auto video = std::make_unique<WaitingSource>();
    auto audio = std::make_unique<WaitingAudioSource>();
    auto& v = *video;
    auto& a = *audio;
    rtsp::MediaSession media(std::move(video), std::move(audio), timeline);
    Peer peer(media.description().tracks[0]);
    peer.subscription->add_track(1, media.description().tracks[1], {2, 3}, 100, 2);
    check(media.subscribe(peer.subscription, "PLAY") == rtsp::SubscribeResult::Started, "PLAY failed");
    rtsp::net::OutboundMessage message;
    check(peer.queue->try_pop(message) && message.wire == "PLAY", "PLAY response missing");
    media.tick(t0);
    check(media.next_deadline() == t0 + 5ms && !media.ended() && peer.queue->empty(),
          "temporary lack of input blocked or stopped a live session");
    a.ready = true;
    a.pts = 4800;  // Later than the video frame that has not arrived yet.
    media.tick(t0 + 5ms);
    check(media.next_deadline() == t0 + 10ms && peer.queue->empty(),
          "a future AAC AU hid the video input until the AAC deadline");
    v.ready = true;
    media.tick(t0 + 150ms);
    check(peer.queue->try_pop(message) && message.kind == rtsp::net::OutboundKind::Rtp,
          "live sequence did not recover after waiting");
    media.stop();
    media.stop();
    check(media.ended() && v.stopped && a.stops == 1, "stop did not release both sources once");
}

}  // namespace

int main() {
    try {
        test_wait_and_validate_sources();
        test_capture_timeline();
        test_live_backlog_and_resume();
        test_required_live_source_failure();
        std::cout << "PASS: live wait, shared timeline, backlog, resume and capture failure\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
