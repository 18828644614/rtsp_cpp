#include "media/media_session.h"
#include "rtsp/rtsp_server_connection.h"
#include "rtsp/rtsp_server_session.h"
#include "rtsp/rtsp_stream_parser.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
const std::string video_path = "multitrack_test.h264";
const std::string audio_path = "multitrack_test.aac";

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void save_samples(int video_count, int audio_count, int idr_interval = 10) {
    std::ofstream video(video_path, std::ios::binary);
    const std::string prefix("\0\0\0\1", 4);
    for (int i = 0; i < video_count; ++i) {
        video << prefix << std::string("\x09\xf0", 2);
        if (i == 0) {
            video << prefix << std::string("\x67\x64\0\x1f", 4);
            video << prefix << std::string("\x68\xee", 2);
        }
        video << prefix << static_cast<char>(i % idr_interval == 0 ? 0x65 : 0x41)
              << static_cast<char>(0x30 + i);
    }
    std::ofstream audio(audio_path, std::ios::binary);
    for (int i = 0; i < audio_count; ++i) {
        audio << std::string("\xff\xf1\x4c\x80\x01\x5f\xfc", 7)
              << static_cast<char>(i) << std::string("\x22\x33", 2);
    }
    check(static_cast<bool>(video) && static_cast<bool>(audio), "could not save multitrack sample");
}

struct Packet {
    unsigned int channel = 0;
    rtsp::RtpPacket rtp;
};

Packet decode(const std::string& wire) {
    rtsp::RtspStreamParser parser;
    parser.append(wire.data(), wire.size());
    rtsp::RtspStreamItem item;
    check(parser.next(item) && item.is_interleaved, "expected complete interleaved RTP");
    Packet packet;
    packet.channel = item.channel;
    packet.rtp = rtsp::parse_rtp_packet(item.payload);
    parser.finish();
    return packet;
}

struct Peer {
    Peer(const rtsp::SessionDescription& description, bool video = true, bool audio = true,
         int channel = 0, std::uint32_t ssrc = 31)
        : queue(std::make_shared<rtsp::net::OutboundQueue>()),
          subscription(std::make_shared<rtsp::MediaSubscription>(queue)) {
        for (std::size_t i = 0; i < description.tracks.size(); ++i) {
            const rtsp::MediaTrack& track = description.tracks[i];
            if ((track.media_type == "video" && video) || (track.media_type == "audio" && audio)) {
                rtsp::TcpTransport transport;
                transport.rtp_channel = channel + static_cast<int>(i) * 2;
                transport.rtcp_channel = transport.rtp_channel + 1;
                subscription->add_track(i, track, transport, 100, ssrc + static_cast<std::uint32_t>(i));
            }
        }
    }
    std::shared_ptr<rtsp::net::OutboundQueue> queue;
    std::shared_ptr<rtsp::MediaSubscription> subscription;
};

bool pop(Peer& peer, rtsp::net::OutboundMessage& message) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (peer.queue->bytes() == 0 && !peer.queue->closed()) {
        check(Clock::now() < deadline, "multitrack queue timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return peer.queue->pop(message);
}

void play(rtsp::MediaSession& media, Peer& peer) {
    check(media.subscribe(peer.subscription, "PLAY 200") == rtsp::SubscribeResult::Started,
          "multitrack PLAY failed");
    rtsp::net::OutboundMessage response;
    check(pop(peer, response) && response.kind == rtsp::net::OutboundKind::RtspResponse
              && response.wire == "PLAY 200", "media preceded PLAY response");
}

std::vector<Packet> collect(Peer& peer) {
    std::vector<Packet> packets;
    rtsp::net::OutboundMessage message;
    while (pop(peer, message)) {
        packets.push_back(decode(message.wire));
    }
    check(!peer.subscription->failed(), "multitrack subscription failed");
    return packets;
}

// 同时检查两轨的通道、序号、SSRC、媒体顺序、AU Header 和共同时间起点。
void check_packets(const std::vector<Packet>& packets, int video_count, int audio_count,
                   int channel, std::uint32_t ssrc, int start_video = 0) {
    int videos = 0;
    int audios = 0;
    std::uint16_t video_sequence = 100;
    std::uint16_t audio_sequence = 100;
    std::uint64_t last_time = 0;
    for (const Packet& packet : packets) {
        const rtsp::RtpPacket& rtp = packet.rtp;
        std::uint64_t media_time = 0;
        if (rtp.payload_type == 96) {
            check(packet.channel == static_cast<unsigned int>(channel)
                      && rtp.ssrc == ssrc && rtp.sequence_number == video_sequence++, "wrong video RTP state");
            check(rtp.timestamp == static_cast<std::uint32_t>(videos * 3600), "wrong video timestamp");
            media_time = (static_cast<std::uint64_t>(rtp.timestamp) + start_video * 3600) * 1000000 / 90000;
            if (rtp.marker) {
                check((static_cast<unsigned char>(rtp.payload[0]) & 31) == 1
                          || (static_cast<unsigned char>(rtp.payload[0]) & 31) == 5,
                      "video marker appeared before picture");
                ++videos;
            }
        } else {
            check(rtp.payload_type == 97 && packet.channel == static_cast<unsigned int>(channel + 2)
                      && rtp.ssrc == ssrc + 1 && rtp.sequence_number == audio_sequence++, "wrong audio RTP state");
            check(rtp.marker && rtp.payload.size() == 7
                      && rtp.payload.substr(0, 4) == std::string("\0\x10\0\x18", 4), "wrong AAC AU header");
            const unsigned int index = static_cast<unsigned char>(rtp.payload[4]);
            check(rtp.timestamp == index * 1024 - static_cast<unsigned int>(start_video * 1920),
                  "audio lost common video time origin");
            media_time = static_cast<std::uint64_t>(index) * 1024 * 1000000 / 48000;
            if (video_count != 0) {
                check(videos > 0, "late subscriber received audio before video IDR");
            }
            ++audios;
        }
        check(media_time >= last_time, "scheduler did not order tracks by media time");
        last_time = media_time;
    }
    check(videos == video_count && audios == audio_count, "a track was truncated at the other track EOF");
}

void test_shared_tracks_and_late_join() {
    save_samples(24, 60);
    rtsp::MediaSession media(video_path, audio_path);
    check(std::remove(video_path.c_str()) == 0 && std::remove(audio_path.c_str()) == 0,
          "shared files were not closed after initial load");
    Peer first(media.description());
    play(media, first);
    std::vector<Packet> first_packets;
    for (;;) {
        rtsp::net::OutboundMessage message;
        check(pop(first, message), "first peer ended before late join");
        Packet packet = decode(message.wire);
        first_packets.push_back(packet);
        if (packet.rtp.payload_type == 96 && packet.rtp.marker && packet.rtp.timestamp == 3600) {
            break;
        }
    }
    Peer late(media.description(), true, true, 6, 41);
    Peer audio_only(media.description(), false, true, 10, 51);
    play(media, audio_only);
    play(media, late);
    const std::vector<Packet> late_packets = collect(late);
    check(!late_packets.empty() && late_packets[0].rtp.payload_type == 96,
          "late join did not begin with video");
    int start_video = -1;
    for (const Packet& packet : late_packets) {
        if (packet.rtp.payload_type == 96 && packet.rtp.marker) {
            check((static_cast<unsigned char>(packet.rtp.payload[0]) & 31) == 5, "late join missed IDR");
            start_video = static_cast<unsigned char>(packet.rtp.payload[1]) - 0x30;
            break;
        }
    }
    check(start_video >= 10 && start_video % 10 == 0, "late join used an earlier or non-IDR frame");
    const int first_audio = (start_video * 1920 + 1023) / 1024;
    check_packets(late_packets, 24 - start_video, 60 - first_audio, 6, 41, start_video);
    const auto remaining = collect(first);
    first_packets.insert(first_packets.end(), remaining.begin(), remaining.end());
    check_packets(first_packets, 24, 60, 0, 31);
    const auto audio_packets = collect(audio_only);
    check(!audio_packets.empty(), "late audio-only subscriber waited for video IDR");
    const unsigned int first_audio_index = static_cast<unsigned char>(audio_packets[0].rtp.payload[4]);
    check(first_audio_index >= 2 && audio_packets.size() == 60 - first_audio_index,
          "late audio-only subscriber did not join shared progress");
    for (std::size_t i = 0; i < audio_packets.size(); ++i) {
        check(audio_packets[i].channel == 12 && audio_packets[i].rtp.ssrc == 52
                  && audio_packets[i].rtp.timestamp == i * 1024,
              "late audio-only subscriber did not use its own RTP origin");
    }
}

void test_cancel_and_no_later_idr() {
    save_samples(8, 20, 1);
    rtsp::MediaSession media(video_path, audio_path);
    Peer cancelled(media.description());
    play(media, cancelled);
    Peer healthy(media.description(), true, true, 6, 41);
    play(media, healthy);
    cancelled.subscription->cancel();
    const auto queued = cancelled.queue->bytes();
    check(!collect(healthy).empty(), "cancelling one peer stopped other tracks");
    check(cancelled.queue->bytes() == queued && !cancelled.queue->closed(),
          "cancelled subscription received more media or lost its response queue");

    save_samples(8, 20, 100);
    rtsp::MediaSession no_idr(video_path, audio_path);
    Peer first(no_idr.description());
    play(no_idr, first);
    for (;;) {
        rtsp::net::OutboundMessage message;
        check(pop(first, message), "no-IDR fixture ended too early");
        const auto packet = decode(message.wire);
        if (packet.rtp.payload_type == 96 && packet.rtp.marker) {
            break;
        }
    }
    Peer waiting(no_idr.description());
    play(no_idr, waiting);
    check(collect(waiting).empty(), "audio escaped the wait for video IDR");
}

void test_track_selection_and_short_audio() {
    save_samples(8, 3);
    rtsp::MediaSession media(video_path, audio_path);
    Peer video_only(media.description(), true, false);
    play(media, video_only);
    check_packets(collect(video_only), 8, 0, 0, 31);
    rtsp::MediaSession audio_selection(video_path, audio_path);
    Peer audio_only(audio_selection.description(), false, true);
    play(audio_selection, audio_only);
    // 音频订阅无需等待视频 IDR；这里与视频一起从时间零点启动。
    const auto audio = collect(audio_only);
    check(audio.size() == 3, "audio-only subscriber missed early AU");
    for (std::size_t i = 0; i < audio.size(); ++i) {
        check(audio[i].rtp.payload_type == 97 && audio[i].channel == 2
                  && audio[i].rtp.timestamp == i * 1024, "audio-only track used video clock");
    }

    rtsp::MediaSession pure_audio("", audio_path);
    Peer peer(pure_audio.description(), false, true);
    play(pure_audio, peer);
    const auto packets = collect(peer);
    check(packets.size() == 3 && packets[0].channel == 0
              && packets[0].rtp.timestamp == 0 && packets[2].rtp.timestamp == 2048,
          "pure audio resource failed");
}

rtsp::RtspStreamItem receive_item(rtsp::net::TcpSocket& socket, rtsp::RtspStreamParser& parser) {
    rtsp::RtspStreamItem item;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!parser.next(item)) {
        check(Clock::now() < deadline, "TCP multitrack response timed out");
        if (socket.wait_readable(20)) {
            char buffer[4096];
            const auto count = socket.receive(buffer, sizeof(buffer));
            check(count != 0, "TCP ended before expected response");
            parser.append(buffer, count);
        }
    }
    return item;
}

rtsp::RtspMessage exchange(rtsp::net::TcpSocket& socket, rtsp::RtspStreamParser& parser,
                           const std::string& method, const std::string& path,
                           int cseq, const std::string& headers = "") {
    const std::string wire = method + " rtsp://127.0.0.1/live" + path
        + " RTSP/1.0\r\nCSeq: " + std::to_string(cseq) + "\r\n" + headers + "\r\n";
    socket.send_all(wire.data(), wire.size());
    const auto item = receive_item(socket, parser);
    check(!item.is_interleaved && item.message.header("CSeq") == std::to_string(cseq),
          "media preceded RTSP response");
    return item.message;
}

void test_tcp_two_setup_and_media() {
    save_samples(8, 20);
    auto media = std::make_shared<rtsp::MediaSession>(video_path, audio_path);
    rtsp::net::TcpSocket listener;
    listener.listen("127.0.0.1", 0);
    rtsp::net::TcpSocket socket;
    socket.connect("127.0.0.1", listener.local_port());
    rtsp::RtspServerConnection connection(media);
    connection.accept_from(listener);
    connection.start();
    rtsp::RtspStreamParser parser;
    const auto describe = exchange(socket, parser, "DESCRIBE", "", 1);
    check(describe.status_code == 200 && rtsp::parse_sdp(describe.body).tracks.size() == 2,
          "DESCRIBE did not return two usable tracks");
    const auto video = exchange(socket, parser, "SETUP", "/trackID=0", 2,
        "Transport: RTP/AVP/TCP;unicast;interleaved=6-7\r\n");
    check(video.status_code == 200, "video SETUP failed");
    const std::string session = "Session: " + video.header("Session") + "\r\n";
    check(exchange(socket, parser, "SETUP", "/trackID=1", 3,
        "Transport: RTP/AVP/TCP;unicast;interleaved=10-11\r\n").status_code == 454,
        "second SETUP accepted missing Session");
    check(exchange(socket, parser, "SETUP", "/trackID=1", 4, session
        + "Transport: RTP/AVP/TCP;unicast;interleaved=7-8\r\n").status_code == 461,
        "overlapping RTP/RTCP channels were accepted");
    const auto audio = exchange(socket, parser, "SETUP", "/trackID=1", 5, session
        + "Transport: RTP/AVP/TCP;unicast;interleaved=8-9\r\n");
    check(audio.status_code == 200 && audio.header("Session") == video.header("Session"),
          "second track changed Session or was rejected");
    check(exchange(socket, parser, "PLAY", "", 6, session).status_code == 200, "two-track PLAY failed");

    std::vector<Packet> packets;
    char buffer[4096];
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    for (;;) {
        rtsp::RtspStreamItem item;
        while (parser.next(item)) {
            check(item.is_interleaved, "unexpected RTSP message during media");
            Packet packet;
            packet.channel = item.channel;
            packet.rtp = rtsp::parse_rtp_packet(item.payload);
            packets.push_back(packet);
        }
        check(Clock::now() < deadline, "TCP media did not finish");
        if (!socket.wait_readable(20)) {
            continue;
        }
        const auto count = socket.receive(buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        parser.append(buffer, count);
    }
    parser.finish();
    connection.join();
    check(connection.finished() && !packets.empty(), "connection did not release after EOF");
    check_packets(packets, 8, 20, 6, packets[0].rtp.ssrc);
}

}  // namespace

int main() {
    try {
        test_shared_tracks_and_late_join();
        test_track_selection_and_short_audio();
        test_cancel_and_no_later_idr();
        test_tcp_two_setup_and_media();
        std::remove(video_path.c_str());
        std::remove(audio_path.c_str());
        std::cout << "PASS: TCP two-track SDP/SETUP, independent RTP, shared timeline, late IDR and per-track EOF\n";
        return 0;
    } catch (const std::exception& error) {
        std::remove(video_path.c_str());
        std::remove(audio_path.c_str());
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
