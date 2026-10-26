#include "media/media_session.h"
#include "rtsp/rtsp_server_connection.h"
#include "rtsp/rtsp_stream_parser.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
const std::string input_path = "media_session_test.h264";

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
void wait_for(Predicate ready) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!ready()) {
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
    explicit Peer(std::size_t limit = 1024 * 1024, int channel = 0,
                  std::uint16_t sequence = 100, std::uint32_t ssrc = 1)
        : queue(std::make_shared<rtsp::net::OutboundQueue>(limit)) {
        rtsp::TcpTransport tcp;
        tcp.rtp_channel = channel;
        tcp.rtcp_channel = channel + 1;
        subscription = std::make_shared<rtsp::MediaSubscription>(
            queue, rtsp::MediaTransport(tcp), 96, sequence, ssrc);
    }
    std::shared_ptr<rtsp::net::OutboundQueue> queue;
    std::shared_ptr<rtsp::MediaSubscription> subscription;
};

rtsp::net::OutboundMessage pop(Peer& peer) {
    wait_for([&] { return peer.queue->bytes() != 0 || peer.queue->closed(); });
    rtsp::net::OutboundMessage message;
    check(peer.queue->pop(message), "unexpected end of queue");
    return message;
}

void play(rtsp::MediaSession& media, Peer& peer) {
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
    rtsp::MediaSession media(input_path);
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
    wait_for([&] { return media.ended() && a.queue->closed() && b.queue->closed(); });
    check(!a.subscription->failed() && !b.subscription->failed(), "normal EOF reported failure");
    Peer after_eof;
    check(media.subscribe(after_eof.subscription, "PLAY 200") == rtsp::SubscribeResult::Ended
              && after_eof.queue->bytes() == 0, "EOF resource restarted or emitted 200");
}

void test_pause_cancel_and_stop() {
    save_sample(10, 1);
    rtsp::MediaSession media(input_path);
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
    rtsp::MediaSession media(input_path);
    Peer healthy;
    play(media, healthy);
    std::uint16_t seq = 100;
    check(read_frame(healthy, seq, 0, 1).index == 0, "healthy client missed start");
    Peer slow(32);
    play(media, slow);
    wait_for([&] { return slow.subscription->failed(); });
    for (int index = 1; index < 8; ++index) {
        check(read_frame(healthy, seq, 0, 1).index == index, "slow peer affected healthy client");
    }
    wait_for([&] { return media.ended(); });
    check(!healthy.subscription->failed(), "slow peer ended shared source");

    save_sample(4, 10);  // 只有开头一个 IDR，晚加入者一直等到 EOF。
    rtsp::MediaSession no_more_idr(input_path);
    Peer a;
    play(no_more_idr, a);
    seq = 100;
    read_frame(a, seq, 0, 1);
    Peer waiting;
    play(no_more_idr, waiting);
    wait_for([&] { return waiting.queue->closed(); });
    check(waiting.queue->bytes() == 0 && waiting.subscription->finished(),
          "waiting peer received P frames or remained subscribed at EOF");
}

void test_failed_admission_and_idle_stop() {
    save_sample(4, 1);
    rtsp::MediaSession media(input_path);
    Peer closed;
    closed.queue->close();
    check(media.subscribe(closed.subscription, "PLAY 200") == rtsp::SubscribeResult::QueueClosed,
          "closed queue accepted subscription");
    Peer valid;
    check(media.subscribe(valid.subscription, "PLAY 200", true) == rtsp::SubscribeResult::Started,
          "failed admission advanced source");
    media.stop();
    check(valid.queue->closed(), "stopping immediately after PLAY leaked subscription");

    rtsp::MediaSession idle(input_path);
    idle.stop();
    check(idle.ended(), "idle scheduler did not wake on stop");
}

// 实际启动连接的接收/发送线程，验证它们在控制阶段和播放阶段都能被安全回收。
void test_connection_stop(bool playing) {
    save_sample(20, 5);
    auto media = std::make_shared<rtsp::MediaSession>(input_path);
    rtsp::net::TcpSocket listener;
    listener.listen("127.0.0.1", 0);
    rtsp::net::TcpSocket client;
    client.connect("127.0.0.1", listener.local_port());
    rtsp::RtspServerConnection connection(media);
    connection.accept_from(listener);
    connection.start();

    if (playing) {
        rtsp::RtspStreamParser parser;
        auto response = [&] {
            rtsp::RtspStreamItem item;
            char buffer[4096];
            const auto deadline = Clock::now() + std::chrono::seconds(3);
            while (!parser.next(item)) {
                check(Clock::now() < deadline, "connection response timed out");
                if (client.wait_readable(50)) {
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
    connection.stop();
    connection.join();
    check(connection.finished() && Clock::now() - start < std::chrono::seconds(2),
          "connection stop leaked or blocked a thread");
    media->stop();
}

}  // namespace

int main() {
    try {
        test_shared_progress_and_late_join();
        test_pause_cancel_and_stop();
        test_slow_peer_and_waiting_at_eof();
        test_failed_admission_and_idle_stop();
        test_connection_stop(false);
        test_connection_stop(true);
        std::remove(input_path.c_str());
        std::cout << "PASS: shared source, late IDR, independent RTP, pause, cancellation, slow peer and EOF\n";
        return 0;
    } catch (const std::exception& error) {
        std::remove(input_path.c_str());
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
