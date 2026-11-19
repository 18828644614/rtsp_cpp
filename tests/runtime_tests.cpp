#include "event_test_driver.h"
#include "media/media_clock.h"
#include "rtp/rtcp_packet.h"
#include "rtsp/rtsp_server.h"

#include <iostream>

namespace {

using Clock = std::chrono::steady_clock;
using rtsp::net::OutboundKind;
using rtsp::net::OutboundMessage;
void check(bool valid, const char* message) {
    if (!valid) {
        throw std::runtime_error(message);
    }
}

template <typename Action>
void rejects(Action action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, message);
}

void put32(std::string& wire, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        wire += static_cast<char>((value >> shift) & 255);
    }
}

std::string rr(std::uint32_t source) {
    // 独立固定接收报告：reporter=99，fraction lost=7，cumulative lost=-2。
    std::string wire("\x81\xc9\0\x07", 4);
    put32(wire, 99);
    put32(wire, source);
    wire.append("\x07\xff\xff\xfe", 4);
    put32(wire, 1234);
    put32(wire, 42);
    put32(wire, 0x12345678);
    put32(wire, 65536);
    return wire;
}

rtsp::RtspStreamItem decode(const std::string& wire) {
    rtsp::RtspStreamParser parser;
    parser.append(wire.data(), wire.size());
    rtsp::RtspStreamItem item;
    check(parser.next(item), "incomplete outbound item");
    parser.finish();
    return item;
}

void test_rtcp_codec_and_clock() {
    const auto wire = rtsp::make_rtcp_sender_report(0x01020304, 0x1122334455667788ULL,
                                                   0xaabbccdd, 2, 300, "x", true);
    const std::string expected(
        "\x80\xc8\0\x06\x01\x02\x03\x04\x11\x22\x33\x44\x55\x66\x77\x88"
        "\xaa\xbb\xcc\xdd\0\0\0\x02\0\0\x01\x2c"
        "\x81\xca\0\x02\x01\x02\x03\x04\x01\x01x\0"
        "\x81\xcb\0\x01\x01\x02\x03\x04", 48);
    check(wire == expected, "SR/SDES/BYE differs from independent network bytes");
    const auto packets = rtsp::parse_rtcp_compound(wire);
    check(packets.size() == 3 && packets[0].ssrc == 0x01020304
        && packets[0].ntp == 0x1122334455667788ULL && packets[0].rtp_timestamp == 0xaabbccdd
        && packets[0].packet_count == 2 && packets[0].octet_count == 300
        && packets[1].cname == "x" && packets[2].bye_sources[0] == 0x01020304,
        "RTCP fields did not survive parsing");
    const auto feedback = rtsp::parse_rtcp_compound(rr(31))[0];
    check(feedback.ssrc == 99 && feedback.reports[0].ssrc == 31
        && feedback.reports[0].fraction_lost == 7 && feedback.reports[0].cumulative_lost == -2
        && feedback.reports[0].highest_sequence == 1234 && feedback.reports[0].jitter == 42
        && feedback.reports[0].last_sr == 0x12345678 && feedback.reports[0].delay_since_last_sr == 65536,
        "RR report block decoded incorrectly");
    for (std::size_t length = 1; length < 28; ++length) {
        rejects([&] {
            rtsp::parse_rtcp_compound(wire.substr(0, length));
        },
                "truncated SR accepted");
    }
    const std::vector<std::string> bad = {
        "", std::string("\x40\xc9\0\x01\0\0\0\1", 8),
        std::string("\x81\xc9\0\x01\0\0\0\1", 8),
        std::string("\xa0\xc9\0\x01\0\0\0\0", 8),
        std::string("\xa0\xc9\0\x01\0\0\0\x05", 8),
        std::string("\x81\xca\0\x02\0\0\0\1\1\x05x\0", 12),
        wire + "x"
    };
    for (const auto& invalid : bad) {
        rejects([&] {
            rtsp::parse_rtcp_compound(invalid);
        },
                "invalid RTCP accepted");
    }
    // 合法末包 padding，不允许 padding 出现在 compound 中间。
    std::string padded("\xa0\xc9\0\x02\0\0\0\1\0\0\0\x04", 12);
    check(rtsp::parse_rtcp_compound(padded).size() == 1, "valid RTCP padding rejected");
    rejects([&] {
        rtsp::parse_rtcp_compound(padded + wire);
    },
            "non-final padding accepted");

    const auto origin = Clock::now();
    rtsp::MediaClock clock(origin, std::chrono::system_clock::time_point{});
    check(clock.ntp_at(origin) == (2208988800ULL << 32), "wrong NTP epoch");
    check(clock.ntp_at(origin + std::chrono::milliseconds(500))
              == ((2208988800ULL << 32) | 0x80000000ULL), "wrong NTP fractional seconds");
    check(rtsp::MediaClock::rtp_at(0xfffffff0, origin, 90000, origin + std::chrono::seconds(1))
              == static_cast<std::uint32_t>(0xfffffff0U + 90000U), "RTP wraparound failed");
    check(rtsp::MediaClock::rtp_at(0, origin, 48000, origin + std::chrono::milliseconds(500)) == 24000,
          "audio reused video clock rate");
}

void test_session_headers_and_keepalive() {
    const auto parsed = rtsp::parse_session_header(" 123 ;unknown=yes; TIMEOUT = 2 ");
    check(parsed.id == "123" && parsed.timeout_seconds == 2U, "Session timeout was not parsed");
    for (const std::string invalid : {"", "a b", "1;timeout=0", "1;timeout=-1", "1;timeout=",
                                      "1;timeout=9999999999999", "1;timeout=2;Timeout=3"}) {
        rejects([&] {
            rtsp::parse_session_header(invalid);
        },
                "invalid Session timeout accepted");
    }
    rtsp::RtspServerSession session("123", rtsp::SessionDescription(), 2);
    rtsp::RtspMessage request;
    request.method = "GET_PARAMETER";
    request.uri = "rtsp://127.0.0.1/live";
    request.set_header("CSeq", "1");
    check(session.handle(request).status_code == 454, "keepalive accepted without Session");
    request.method = "SETUP";
    request.uri += "/trackID=0";
    request.set_header("Transport", "RTP/AVP/TCP;unicast;interleaved=6-7");
    const auto setup = session.handle(request);
    check(setup.header("Session") == "123;timeout=2", "server did not advertise its timeout");
    request.method = "GET_PARAMETER";
    request.uri = "rtsp://127.0.0.1/live";
    request.set_header("Session", "123;timeout=999");
    check(session.handle(request).status_code == 200 && session.state() == rtsp::SessionReady,
          "empty GET_PARAMETER changed session state");
    request.body = "unsupported";
    check(session.handle(request).status_code == 451, "unsupported parameter was accepted");
}

void test_sent_statistics_and_feedback() {
    const auto start = Clock::now();
    rtsp::MediaSession media(std::make_unique<test::WireVideo>(4), std::make_unique<test::WireAudio>(),
                             rtsp::CaptureTimeline(start), std::chrono::milliseconds{0});
    auto queue = std::make_shared<rtsp::net::OutboundQueue>();
    auto subscription = std::make_shared<rtsp::MediaSubscription>(queue);
    subscription->add_track(0, media.description().tracks[0], {6, 7}, 65535, 31);
    check(subscription->rtp_info("rtsp://127.0.0.1/live/") == "url=rtsp://127.0.0.1/live/trackID=0;seq=65535", "wrong RTP-Info");
    media.subscribe(subscription, "PLAY");
    OutboundMessage message;
    check(queue->try_pop(message) && message.wire == "PLAY", "PLAY response missing");
    media.tick(start);
    check(queue->take_for_send(message) && message.kind == OutboundKind::Rtp, "first RTP missing");
    OutboundMessage report;
    report.kind = OutboundKind::Rtcp;
    report.ssrc = 31;
    const auto before = decode(subscription->prepare_rtcp(report, media.clock(), start));
    check(rtsp::parse_rtcp_compound(before.payload)[0].packet_count == 0,
          "in-flight RTP was counted before completion");
    std::uint32_t packets = 1;
    std::uint32_t octets = static_cast<std::uint32_t>(message.rtp_payload_bytes);
    subscription->on_sent(message);
    queue->complete_send();
    while (queue->take_for_send(message)) {
        check(message.kind == OutboundKind::Rtp, "unexpected first-AU output");
        ++packets;
        octets += static_cast<std::uint32_t>(message.rtp_payload_bytes);
        subscription->on_sent(message);
        queue->complete_send();
    }
    subscription->tick_rtcp(start, std::chrono::seconds(5));
    check(queue->take_for_send(message) && message.kind == OutboundKind::Rtcp, "initial SR missing");
    const auto sr = rtsp::parse_rtcp_compound(decode(subscription->prepare_rtcp(
        message, media.clock(), start + std::chrono::milliseconds(500))).payload)[0];
    check(sr.packet_count == packets && sr.octet_count == octets && sr.rtp_timestamp == 45000,
          "SR counts include headers or timestamp uses last RTP timestamp");
    subscription->tick_rtcp(start + std::chrono::seconds(10), std::chrono::seconds(5));
    check(queue->empty(), "pending RTCP report was queued twice");
    subscription->on_sent(message);
    queue->complete_send();
    subscription->tick_rtcp(start + std::chrono::seconds(4), std::chrono::seconds(5));
    check(queue->empty(), "RTCP interval was ignored");
    subscription->tick_rtcp(start + std::chrono::seconds(5), std::chrono::seconds(5));
    check(queue->take_for_send(message), "periodic SR missing");
    subscription->on_sent(message);
    queue->complete_send();
    check(!subscription->receive_rtcp(6, rr(31)) && !subscription->receive_rtcp(7, rr(32)),
          "wrong channel or SSRC feedback was applied");
    check(subscription->receive_rtcp(7, rr(31)) && subscription->receiver_report(31)->jitter == 42,
          "valid RR was not applied");
    rejects([&] {
        subscription->receive_rtcp(7, rr(31) + "x");
    },
            "truncated feedback accepted");
    media.tick(start + std::chrono::milliseconds(40));
    subscription->end();
    subscription->end();  // 正常收尾幂等，最终报告只能有一份。
    int finals = 0;
    while (queue->take_for_send(message)) {
        if (message.kind == OutboundKind::Rtp) {
            ++packets;
            octets += static_cast<std::uint32_t>(message.rtp_payload_bytes);
        } else {
            const auto final = rtsp::parse_rtcp_compound(decode(subscription->prepare_rtcp(
                message, media.clock(), start + std::chrono::seconds(1))).payload);
            check(final.size() == 3 && final[2].type == 203 && final[0].packet_count == packets
                && final[0].octet_count == octets, "final SR omitted preceding queued RTP");
            ++finals;
        }
        subscription->on_sent(message);
        queue->complete_send();
    }
    check(finals == 1, "duplicate final RTCP report");
}

struct Peer {
    test::DrivenMedia media;
    rtsp::net::TcpSocket listener;
    rtsp::net::TcpSocket socket;
    test::DrivenConnection connection;
    rtsp::RtspStreamParser parser;
    int cseq = 0;
    std::string session;

    explicit Peer(rtsp::RuntimeOptions options = {}, std::size_t nalu_size = 2)
        : media(160, nalu_size), connection(media, options) {
        listener.listen("127.0.0.1", 0);
        socket.connect("127.0.0.1", listener.local_port());
        check(connection.accept_from(listener), "runtime accept failed");
    }

    rtsp::RtspMessage exchange(const std::string& method, const std::string& suffix = "",
                               const std::string& headers = "", const std::string& body = "") {
        const auto text = method + " rtsp://127.0.0.1/live" + suffix + " RTSP/1.0\r\nCSeq: "
            + std::to_string(++cseq) + "\r\n" + headers
            + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        socket.send_all(text.data(), text.size());
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        for (;;) {
            test::pump();
            rtsp::RtspStreamItem item;
            while (parser.next(item)) {
                if (item.is_interleaved) {
                    continue;
                }
                check(item.message.header("CSeq") == std::to_string(cseq), "unexpected response CSeq");
                return item.message;
            }
            check(Clock::now() < deadline, "runtime request timed out");
            if (socket.wait_readable(1)) {
                char buffer[65536];
                const auto count = socket.receive(buffer, sizeof(buffer));
                check(count != 0, "runtime connection closed before response");
                parser.append(buffer, count);
            }
        }
    }

    void setup() {
        const auto response = exchange("SETUP", "/trackID=0",
                                       "Transport: RTP/AVP/TCP;unicast;interleaved=6-7\r\n");
        check(response.status_code == 200, "runtime SETUP failed");
        session = "Session: " + rtsp::session_id_from_header(response.header("Session")) + "\r\n";
    }

    void send_feedback(const std::string& payload) {
        const auto wire = rtsp::serialize_interleaved_frame(7, payload);
        socket.send_all(wire.data(), wire.size());
        rtsp::net::EventLoop loop;
        loop.watch(connection.native_handle(), true, false);
        check(!loop.wait(1000).empty(), "RTCP feedback did not arrive");
        connection.on_readable();
    }
};

void test_connection_expiry() {
    rtsp::RuntimeOptions options;
    options.session_timeout_seconds = 1;
    Peer peer(options);
    peer.setup();
    const auto first_deadline = *peer.connection.next_deadline();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(peer.exchange("GET_PARAMETER", "", peer.session).status_code == 200,
          "connection keepalive rejected");
    const auto renewed = *peer.connection.next_deadline();
    check(renewed > first_deadline, "valid keepalive did not renew Session");
    check(peer.exchange("GET_PARAMETER", "", peer.session, "unknown").status_code == 451,
          "parameter body was accepted");
    check(*peer.connection.next_deadline() == renewed, "invalid request renewed Session");
    check(peer.exchange("OPTIONS").status_code == 200 && *peer.connection.next_deadline() == renewed,
          "OPTIONS without Session renewed Session");
    peer.socket.send_all("G", 1);
    peer.connection.on_readable();
    check(*peer.connection.next_deadline() == renewed, "partial request renewed Session");
    peer.connection.check_timeout(first_deadline);
    check(!peer.connection.finished(), "renewed Session expired at old deadline");
    peer.connection.check_timeout(renewed + std::chrono::milliseconds(1));
    check(peer.connection.finished(), "expired ready Session leaked connection");

    // 正在向客户端发送 RTP 也必须超时；绝对收尾期限不受发送进展刷新。
    Peer playing(options);
    playing.setup();
    check(playing.exchange("PLAY", "", playing.session).status_code == 200, "runtime PLAY failed");
    const auto expiry = Clock::now() + std::chrono::seconds(2);
    playing.connection.check_timeout(expiry);
    check(!playing.connection.wants_read(), "outgoing RTP kept expired Session alive");
    playing.connection.check_timeout(expiry + options.drain_timeout);
    check(playing.connection.finished(), "draining expired Session exceeded absolute deadline");
}

void test_rtcp_feedback_renews_connection() {
    rtsp::RuntimeOptions options;
    options.session_timeout_seconds = 1;
    Peer peer(options);
    peer.setup();
    peer.exchange("PLAY", "", peer.session);
    std::uint32_t ssrc = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (ssrc == 0) {
        test::pump();
        rtsp::RtspStreamItem item;
        while (peer.parser.next(item)) {
            if (item.is_interleaved && item.channel == 6) {
                ssrc = rtsp::parse_rtp_packet(item.payload).ssrc;
            }
        }
        check(Clock::now() < deadline, "no RTP for feedback test");
        if (ssrc == 0 && peer.socket.wait_readable(1)) {
            char buffer[65536];
            const auto count = peer.socket.receive(buffer, sizeof(buffer));
            check(count != 0, "feedback connection closed");
            peer.parser.append(buffer, count);
        }
    }
    // 排空首次 SR，保证 next_deadline 返回 Session 期限，而不是立即发送报告的期限。
    for (int i = 0; i < 3; ++i) {
        test::pump();
    }
    const auto original = *peer.connection.next_deadline();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    peer.send_feedback(rr(ssrc));
    check(*peer.connection.next_deadline() > original, "matching RR did not renew Session deadline");
    const auto renewed = *peer.connection.next_deadline();
    peer.send_feedback(rr(ssrc + 1));
    check(*peer.connection.next_deadline() == renewed, "unrelated RR renewed Session deadline");
    peer.connection.check_timeout(original);
    check(peer.connection.wants_read(), "valid RR did not keep connection open");
    peer.send_feedback("x");
    check(peer.connection.finished(), "malformed RTCP was silently accepted");
}

void test_partial_rtp_counts_and_teardown() {
    rtsp::RuntimeOptions options;
    options.rtcp_interval = std::chrono::milliseconds(10);
    Peer peer(options, 512 * 1024);
    int small_buffer = 1024;
    check(::setsockopt(peer.connection.native_handle(), SOL_SOCKET, SO_SNDBUF,
                      reinterpret_cast<const char*>(&small_buffer), sizeof(small_buffer)) == 0,
          "could not constrain runtime send buffer");
    peer.setup();
    peer.exchange("PLAY", "", peer.session);
    // 先让 64 KiB 发送预算和内核背压生效，再发 TEARDOWN；媒体帧必须完整排空。
    const auto request = "TEARDOWN rtsp://127.0.0.1/live RTSP/1.0\r\nCSeq: 3\r\n" + peer.session + "\r\n";
    peer.socket.send_all(request.data(), request.size());
    std::uint32_t packets = 0;
    std::uint32_t octets = 0;
    int finals = 0;
    int responses = 0;
    bool saw_rtcp = false;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    for (;;) {
        test::pump();
        rtsp::RtspStreamItem item;
        while (peer.parser.next(item)) {
            if (!item.is_interleaved) {
                check(item.message.header("CSeq") == "3" && item.message.status_code == 200,
                      "TEARDOWN response missing after partial writes");
                ++responses;
            } else if (item.channel == 6) {
                const auto rtp = rtsp::parse_rtp_packet(item.payload);
                ++packets;
                octets += static_cast<std::uint32_t>(rtp.payload.size());
            } else {
                check(item.channel == 7, "partial writes changed RTCP channel");
                const auto compound = rtsp::parse_rtcp_compound(item.payload);
                check(compound[0].packet_count == packets && compound[0].octet_count == octets,
                      "partial-send SR counted incomplete or future RTP packets");
                saw_rtcp = true;
                if (compound.size() == 3) {
                    ++finals;
                }
            }
        }
        check(Clock::now() < deadline, "partial-write TEARDOWN did not drain");
        if (peer.socket.wait_readable(1)) {
            char buffer[65536];
            const auto count = peer.socket.receive(buffer, sizeof(buffer));
            if (count == 0) {
                break;
            }
            peer.parser.append(buffer, count);
        }
    }
    peer.parser.finish();
    check(packets > 100 && saw_rtcp && finals == 1 && responses == 1 && peer.connection.finished(),
          "partial-write TEARDOWN lost RTP, final report or response");
}

void test_graceful_stop_reports() {
    rtsp::RtspServer server("127.0.0.1", 0, std::make_unique<test::WireVideo>(100),
                            std::make_unique<test::WireAudio>(), rtsp::CaptureTimeline{});
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            server.run();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    try {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (server.local_port() == 0) {
            check(Clock::now() < deadline, "stop server did not start");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        rtsp::net::TcpSocket socket;
        socket.connect("127.0.0.1", server.local_port());
        rtsp::RtspStreamParser parser;
        int cseq = 0;
        const auto exchange = [&](const std::string& method, const std::string& suffix,
                                   const std::string& headers) {
            const auto text = method + " rtsp://127.0.0.1/live" + suffix + " RTSP/1.0\r\nCSeq: "
                + std::to_string(++cseq) + "\r\n" + headers + "\r\n";
            socket.send_all(text.data(), text.size());
            for (;;) {
                rtsp::RtspStreamItem item;
                if (parser.next(item)) {
                    check(!item.is_interleaved && item.message.status_code == 200,
                          "stop test request failed");
                    return item.message;
                }
                check(socket.wait_readable(2000), "stop test response timed out");
                char buffer[65536];
                const auto count = socket.receive(buffer, sizeof(buffer));
                check(count != 0, "stop test response truncated");
                parser.append(buffer, count);
            }
        };
        const auto setup = exchange("SETUP", "/trackID=0",
                                    "Transport: RTP/AVP/TCP;unicast;interleaved=6-7\r\n");
        exchange("PLAY", "", "Session: " + rtsp::session_id_from_header(setup.header("Session")) + "\r\n");
        bool stopped = false;
        int finals = 0;
        std::uint32_t packets = 0;
        for (;;) {
            rtsp::RtspStreamItem item;
            while (parser.next(item)) {
                check(item.is_interleaved, "stop produced unexpected RTSP response");
                if (item.channel == 6) {
                    ++packets;
                    if (!stopped) {
                        server.stop();
                        stopped = true;
                    }
                } else {
                    const auto compound = rtsp::parse_rtcp_compound(item.payload);
                    check(compound[0].packet_count == packets, "stop SR omitted preceding RTP");
                    if (compound.size() == 3) {
                        ++finals;
                    }
                }
            }
            check(socket.wait_readable(2500), "graceful stop did not finish");
            char buffer[65536];
            const auto count = socket.receive(buffer, sizeof(buffer));
            if (count == 0) {
                break;
            }
            parser.append(buffer, count);
        }
        parser.finish();
        worker.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        check(stopped && finals == 1 && server.connection_count() == 0,
              "server stop lost BYE or leaked connection");
    } catch (...) {
        server.stop();
        if (worker.joinable()) {
            worker.join();
        }
        throw;
    }
}

}  // namespace

int main() {
    try {
        test_rtcp_codec_and_clock();
        test_session_headers_and_keepalive();
        test_sent_statistics_and_feedback();
        test_connection_expiry();
        test_rtcp_feedback_renews_connection();
        test_partial_rtp_counts_and_teardown();
        test_graceful_stop_reports();
        std::cout << "PASS: RTCP, clocks, RTP-Info, Session expiry, feedback and graceful stop\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
