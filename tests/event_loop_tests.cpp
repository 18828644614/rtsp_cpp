#include "event_test_driver.h"
#include "rtsp/rtsp_server.h"

#include <exception>
#include <iostream>

namespace {

using Clock = rtsp::MediaSession::Clock;
using rtsp::net::TcpSocket;
using rtsp::net::IoStatus;

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
void wait_for(Predicate ready, int seconds = 5) {
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (!ready()) {
        check(Clock::now() < deadline, "event test timed out");
        test::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void test_readiness_and_would_block() {
    TcpSocket listener;
    listener.listen("127.0.0.1", 0);
    listener.set_nonblocking();
    TcpSocket peer;
    check(!listener.try_accept(peer), "empty nonblocking accept did not return");
    TcpSocket client;
    client.connect("127.0.0.1", listener.local_port());
    rtsp::net::EventLoop loop;
    loop.watch(listener.native_handle(), true, false);
    const auto accepted = loop.wait(1000);
    check(accepted.size() == 1 && accepted[0].readable, "listener readiness was lost");
    check(listener.try_accept(peer), "ready accept failed");
    char buffer[16];
    check(peer.receive_some(buffer, sizeof(buffer)).status == IoStatus::WouldBlock,
          "empty receive was confused with EOF");
    loop.clear();
    loop.watch(peer.native_handle(), true, false);
    check(loop.wait(5).empty(), "idle read interest spun without data");
    client.send_all("hello", 5);
    check(loop.wait(1000).size() == 1, "read event did not arrive");
    const auto received = peer.receive_some(buffer, sizeof(buffer));
    check(received.status == IoStatus::Done && std::string(buffer, received.bytes) == "hello",
          "nonblocking receive changed bytes");
    client.shutdown_send();
    check(!loop.wait(1000).empty() && peer.receive_some(buffer, sizeof(buffer)).status == IoStatus::Eof,
          "receive EOF was not distinguishable");
    check(peer.send_some("!", 1).bytes == 1 && client.receive(buffer, sizeof(buffer)) == 1,
          "receive half-close incorrectly disabled sending");

    // watch 本身先检查上限，不依赖真实创建大量操作系统 Socket。
    loop.clear();
    for (std::size_t i = 0; i < rtsp::net::EventLoop::capacity; ++i) {
        loop.watch(static_cast<SOCKET>(i + 1), true, false);
    }
    bool rejected = false;
    try {
        loop.watch(static_cast<SOCKET>(rtsp::net::EventLoop::capacity + 1), true, false);
    } catch (const std::length_error&) {
        rejected = true;
    }
    check(rejected, "select limit was not checked before FD_SET");
}

void test_partial_send_resume() {
    TcpSocket listener;
    listener.listen("127.0.0.1", 0);
    TcpSocket client;
    client.connect("127.0.0.1", listener.local_port());
    TcpSocket sender;
    listener.accept_client(sender);
    sender.set_nonblocking();
    client.set_nonblocking();
    int send_buffer = 1024;
    check(::setsockopt(sender.native_handle(), SOL_SOCKET, SO_SNDBUF,
              reinterpret_cast<const char*>(&send_buffer), sizeof(send_buffer)) == 0,
          "could not shrink send buffer");
    int receive_buffer = 4096;
    check(::setsockopt(client.native_handle(), SOL_SOCKET, SO_RCVBUF,
              reinterpret_cast<const char*>(&receive_buffer), sizeof(receive_buffer)) == 0,
          "could not shrink receive buffer");
    std::string original(8 * 1024 * 1024 + 37, '\0');
    for (std::size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<char>(i % 251);
    }
    std::size_t offset = 0;
    bool blocked = false;
    while (offset < original.size()) {
        const auto result = sender.send_some(original.data() + offset, original.size() - offset);
        if (result.status == IoStatus::WouldBlock) {
            blocked = true;
            break;
        }
        offset += result.bytes;
    }
    check(blocked && offset < original.size(), "test did not exercise send backpressure");
    std::string received;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (received.size() < original.size()) {
        check(Clock::now() < deadline, "partial send did not resume");
        rtsp::net::EventLoop loop;
        loop.watch(client.native_handle(), true, false);
        if (offset < original.size()) {
            loop.watch(sender.native_handle(), false, true);
        }
        for (const auto& event : loop.wait(10)) {
            if (event.socket == sender.native_handle()) {
                const auto result = sender.send_some(original.data() + offset, original.size() - offset);
                offset += result.bytes;
            } else {
                char buffer[65536];
                const auto result = client.receive_some(buffer, sizeof(buffer));
                if (result.status == IoStatus::Done) {
                    received.append(buffer, result.bytes);
                }
            }
        }
    }
    check(offset == original.size() && received == original, "resumed send duplicated or omitted bytes");
}

rtsp::RtspStreamItem receive_item(TcpSocket& client, rtsp::RtspStreamParser& parser) {
    rtsp::RtspStreamItem item;
    wait_for([&] {
        if (parser.next(item)) {
            return true;
        }
        if (client.wait_readable(0)) {
            char buffer[65536];
            const auto count = client.receive(buffer, sizeof(buffer));
            check(count != 0, "connection ended before expected item");
            parser.append(buffer, count);
        }
        return false;
    });
    return item;
}

void send_text(TcpSocket& client, const std::string& text) {
    client.send_all(text.data(), text.size());
}

void test_half_close_and_timeouts() {
    test::DrivenMedia media;
    TcpSocket listener;
    listener.listen("127.0.0.1", 0);
    TcpSocket client;
    client.connect("127.0.0.1", listener.local_port());
    test::DrivenConnection connection(media);
    check(connection.accept_from(listener), "connection accept failed");
    // 半包先到，补齐后再发送第二个请求并半关闭，两个响应都必须排空。
    send_text(client, "OPTIONS rtsp://127.0.0.1/live RTSP/1.0\r\nCSeq: 1\r\n");
    test::pump();
    check(!client.wait_readable(0), "partial request produced a response");
    send_text(client, "\r\nOPTIONS * RTSP/1.0\r\nCSeq: 2\r\n\r\n");
    client.shutdown_send();
    rtsp::RtspStreamParser parser;
    check(receive_item(client, parser).message.header("CSeq") == "1", "first response was lost");
    check(receive_item(client, parser).message.header("CSeq") == "2", "second response was reordered");
    test::wait_finished(connection);
    char buffer[16];
    check(client.wait_readable(1000) && client.receive(buffer, sizeof(buffer)) == 0,
          "drained half-close did not finish");

    TcpSocket idle;
    idle.connect("127.0.0.1", listener.local_port());
    test::DrivenConnection idle_connection(media);
    check(idle_connection.accept_from(listener), "idle accept failed");
    idle_connection.check_timeout(Clock::now() + std::chrono::seconds(61));
    check(idle_connection.finished(), "control idle timeout did not close connection");

    TcpSocket stalled;
    stalled.connect("127.0.0.1", listener.local_port());
    test::DrivenConnection stalled_connection(media);
    check(stalled_connection.accept_from(listener), "stalled accept failed");
    send_text(stalled, "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n");
    wait_for([&] {
        return stalled_connection.wants_write();
    });
    const auto now = Clock::now();
    stalled_connection.check_timeout(now);
    stalled_connection.check_timeout(now + std::chrono::seconds(3));
    check(stalled_connection.finished(), "pending output ignored send-progress timeout");
}

void test_server_stop() {
    rtsp::RtspServer server("127.0.0.1", 0, std::make_unique<test::WireVideo>(0),
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
        wait_for([&] {
            return server.local_port() != 0;
        });
        TcpSocket client;
        client.connect("127.0.0.1", server.local_port());
        wait_for([&] {
            return server.connection_count() == 1;
        });
        std::vector<std::unique_ptr<TcpSocket>> extra;
        for (std::size_t i = 0; i < rtsp::net::EventLoop::capacity - 1; ++i) {
            auto socket = std::make_unique<TcpSocket>();
            socket->connect("127.0.0.1", server.local_port());
            extra.push_back(std::move(socket));
        }
        wait_for([&] {
            return server.connection_count() == rtsp::net::EventLoop::capacity - 1;
        });
        char buffer[1024];
        check(extra.back()->wait_readable(2000) && extra.back()->receive(buffer, sizeof(buffer)) == 0,
              "connection beyond select capacity was not rejected");
        send_text(client, "OPTIONS * RTSP/1.0\r\nCSeq: 99\r\n\r\n");
        check(client.wait_readable(2000), "full connection table blocked existing client");
        std::string response;
        rtsp::RtspMessage message;
        std::size_t consumed = 0;
        while (!rtsp::parse_rtsp_message(response, message, consumed)) {
            check(client.wait_readable(2000), "full-table response was truncated");
            const auto count = client.receive(buffer, sizeof(buffer));
            check(count != 0, "full table closed healthy connection");
            response.append(buffer, count);
        }
        check(message.status_code == 200 && message.header("CSeq") == "99",
              "full connection table corrupted response");
        const auto start = Clock::now();
        server.stop();
        worker.join();
        check(Clock::now() - start < std::chrono::seconds(1) && server.connection_count() == 0,
              "stop did not wake and release event connections");
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (...) {
        server.stop();
        if (worker.joinable()) {
            worker.join();
        }
        throw;
    }
}

void test_media_budget() {
    const auto t0 = Clock::now();
    rtsp::MediaSession media(std::make_unique<test::WireVideo>(64, 2, 0),
                             std::make_unique<test::WireAudio>(), rtsp::CaptureTimeline(t0));
    auto queue = std::make_shared<rtsp::net::OutboundQueue>();
    auto subscription = std::make_shared<rtsp::MediaSubscription>(queue);
    subscription->add_track(0, media.description().tracks[0], {}, 100, 1);
    check(media.subscribe(subscription, "PLAY") == rtsp::SubscribeResult::Started, "PLAY failed");
    rtsp::net::OutboundMessage message;
    check(queue->try_pop(message) && message.wire == "PLAY", "PLAY response missing");
    const auto pop_frames = [&] {
        int frames = 0;
        while (queue->try_pop(message)) {
            check(message.kind == rtsp::net::OutboundKind::Rtp, "expected RTP");
            ++frames;
        }
        return frames;
    };
    const auto due = t0 + std::chrono::milliseconds(150);
    media.tick(due - std::chrono::milliseconds(1));
    check(queue->empty(), "timer delivered media before its deadline");
    media.tick(due);
    check(pop_frames() == 32 && media.next_deadline() == due, "first tick exceeded fairness budget");
    media.tick(due);
    check(pop_frames() == 32 && !media.ended(), "second tick lost pending AUs");
}

}  // namespace

int main() {
    try {
        test_readiness_and_would_block();
        test_partial_send_resume();
        test_half_close_and_timeouts();
        test_media_budget();
        test_server_stop();
        std::cout << "PASS: select, WouldBlock, partial writes, half-close, timeouts, fairness and stop\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
