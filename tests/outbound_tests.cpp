#include "media/h264_file_reader.h"
#include "media/h264_packetizer.h"
#include "net/outbound_queue.h"
#include "rtp/rtp_sender.h"
#include "rtsp/media_transport.h"
#include "rtsp/rtsp_stream_parser.h"

#include <iostream>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_queue() {
    rtsp::net::OutboundQueue queue(5, 2);
    check(queue.push({rtsp::net::OutboundKind::RtspResponse, "abc"}), "first queue push failed");
    check(!queue.push({rtsp::net::OutboundKind::Rtp, "xyz"}), "byte limit was ignored");
    rtsp::net::OutboundMessage message;
    check(queue.pop(message) && message.wire == "abc", "queue order changed");
    queue.close();
    check(!queue.push({rtsp::net::OutboundKind::Rtcp, "x"}), "closed queue accepted data");
    check(!queue.pop(message), "closed empty queue did not finish");

    rtsp::net::OutboundQueue pending(5, 1);
    check(!pending.try_pop(message) && pending.empty(), "empty try_pop did not return immediately");
    check(pending.push({rtsp::net::OutboundKind::Rtp, "abc"})
              && pending.take_for_send(message), "could not acquire pending output");
    check(pending.empty() && pending.bytes() == 3, "in-flight bytes were released too early");
    check(!pending.push({rtsp::net::OutboundKind::Rtp, "x"}), "in-flight message escaped message limit");
    pending.complete_send();
    check(pending.bytes() == 0 && pending.push({rtsp::net::OutboundKind::Rtp, "xyz"}),
          "completed output did not release queue budget");
    pending.close();
    check(pending.try_pop(message) && message.wire == "xyz" && !pending.try_pop(message),
          "closed queue did not drain in order");

    rtsp::net::OutboundQueue byte_limit(5);
    check(byte_limit.push({rtsp::net::OutboundKind::Rtp, "abcd"})
              && byte_limit.take_for_send(message), "could not acquire byte-limit output");
    check(!byte_limit.push({rtsp::net::OutboundKind::Rtp, "xy"}), "in-flight bytes escaped byte limit");
    byte_limit.complete_send();
}

void test_sender() {
    rtsp::TcpTransport tcp;
    tcp.rtp_channel = 6;
    tcp.rtcp_channel = 7;
    rtsp::RtpSender sender(rtsp::MediaTransport(tcp), 96, 100, 0x12345678);
    auto access_unit = std::make_shared<rtsp::H264AccessUnit>();
    access_unit->pts = 9000;
    access_unit->nalus.push_back(std::string("\x65", 1) + std::string(1500, 'x'));
    const auto unit = rtsp::H264Packetizer().packetize_access_unit(access_unit);
    rtsp::net::OutboundQueue queue(10000);
    check(sender.send_payloads(*unit, 0, queue), "RTP sender rejected access unit");

    rtsp::net::OutboundMessage first;
    rtsp::net::OutboundMessage second;
    check(queue.pop(first) && queue.pop(second), "FU-A packets were not queued");
    rtsp::RtspStreamParser parser;
    parser.append(first.wire.data(), first.wire.size());
    rtsp::RtspStreamItem item;
    check(parser.next(item) && item.is_interleaved && item.channel == 6,
          "RTP channel was not transported");
    const rtsp::RtpPacket packet = rtsp::parse_rtp_packet(item.payload);
    check(packet.sequence_number == 100 && packet.timestamp == 0
              && packet.ssrc == 0x12345678 && !packet.marker,
          "first RTP header was wrong");
    parser.finish();
    parser = rtsp::RtspStreamParser();
    parser.append(second.wire.data(), second.wire.size());
    check(parser.next(item) && item.channel == 6, "second RTP frame was not transported");
    const rtsp::RtpPacket last = rtsp::parse_rtp_packet(item.payload);
    check(last.sequence_number == 101 && last.marker, "last FU-A packet marker was wrong");
    parser.finish();
    check(sender.next_sequence() == 102, "RTP sequence was not advanced per packet");
}

}  // namespace

int main() {
    try {
        test_queue();
        test_sender();
        std::cout << "PASS: bounded outbound queue and RTP sender\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
