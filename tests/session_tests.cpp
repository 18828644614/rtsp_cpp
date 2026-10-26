#include "rtsp/rtsp_server_session.h"
#include "rtsp/rtsp_stream_parser.h"
#include "sdp/session_description.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

rtsp::RtspMessage request(const std::string& method, const std::string& uri,
                          const std::string& session_id = "") {
    rtsp::RtspMessage message;
    message.method = method;
    message.uri = uri;
    message.set_header("CSeq", "17");
    if (!session_id.empty()) {
        message.set_header("Session", session_id);
    }
    return message;
}

// 让服务器生成的响应实际经过序列化和流解析，检查 SDP 长度和消息边界。
rtsp::RtspMessage respond(rtsp::RtspServerSession& session, const rtsp::RtspMessage& message) {
    const rtsp::RtspMessage response = session.handle(message);
    const std::string wire = rtsp::serialize_rtsp_message(response);
    rtsp::RtspStreamParser parser;
    rtsp::RtspStreamItem item;
    parser.append(wire.data(), wire.size());
    check(parser.next(item), "server response could not be parsed");
    check(!item.is_interleaved && !item.message.is_request, "server returned a request");
    check(item.message.header("CSeq") == "17", "server did not echo CSeq");
    parser.finish();
    return item.message;
}

void test_transport_and_urls() {
    rtsp::TcpTransport transport;
    check(rtsp::parse_tcp_transport("RTP/AVP/TCP;unicast;interleaved=10-11", transport),
          "valid transport was rejected");
    check(transport.rtp_channel == 10 && transport.rtcp_channel == 11,
          "nondefault channels were lost");
    check(rtsp::make_tcp_transport(transport) == "RTP/AVP/TCP;unicast;interleaved=10-11",
          "transport serialization failed");
    check(rtsp::parse_tcp_transport(
              "rtp/avp/tcp; UNICAST; interleaved=254-255;mode=\"PLAY\"", transport),
          "transport case or mode failed");

    const std::string bad[] = {
        "", "RTP/AVP;unicast;client_port=5000-5001",
        "RTP/AVP/TCP;multicast;interleaved=0-1", "RTP/AVP/TCP;unicast",
        "RTP/AVP/TCP;unicast;interleaved=0-0", "RTP/AVP/TCP;unicast;interleaved=0-256",
        "RTP/AVP/TCP;unicast;interleaved=-1-2", "RTP/AVP/TCP;unicast;interleaved=0-1-2",
        "RTP/AVP/TCP;unicast;interleaved=9999999999999-1",
        "RTP/AVP/TCP;unicast;interleaved=0-1;interleaved=2-3",
        "RTP/AVP/TCP;unicast;unicast;interleaved=0-1",
        "RTP/AVP/TCP;unicast;interleaved=0-1;mode=record",
        "RTP/AVP/TCP;unicast;interleaved=0-1,RTP/AVP;unicast"
    };
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        transport.rtp_channel = 10;
        transport.rtcp_channel = 11;
        check(!rtsp::parse_tcp_transport(bad[i], transport), "invalid transport was accepted");
        check(transport.rtp_channel == 10 && transport.rtcp_channel == 11,
              "failed parsing changed the negotiated channels");
    }
    check(rtsp::session_id_from_header(" 12345 ;timeout=60") == "12345",
          "Session parameters were not removed");

    const std::string origin = "rtsp://127.0.0.1:8554";
    check(rtsp::rtsp_url_path(origin + "/live/") == "/live/", "URL path failed");
    check(rtsp::resolve_control_url(origin + "/live/", "trackID=0")
              == origin + "/live/trackID=0", "relative control failed");
    check(rtsp::resolve_control_url(origin + "/live", "trackID=0")
              == origin + "/trackID=0", "base filename was not replaced");
    check(rtsp::resolve_control_url(origin + "/live/", "/other/track")
              == origin + "/other/track", "root control failed");
    check(rtsp::resolve_control_url(origin + "/live/", origin + "/absolute")
              == origin + "/absolute", "absolute control failed");
    check(rtsp::resolve_control_url(origin + "/live/", "*") == origin + "/live/",
          "aggregate control failed");
    check(rtsp::resolve_control_url(origin, "trackID=0") == origin + "/trackID=0",
          "origin-only base failed");
    check(rtsp::resolve_control_url(origin + "/live/?token=1", "trackID=0")
              == origin + "/live/trackID=0", "base query was not removed");

    const std::string bad_controls[] = {"../track", "track/..", "/live/./track", "track name",
                                         "http://example.com/track", origin + "/live/../track"};
    for (std::size_t i = 0; i < sizeof(bad_controls) / sizeof(bad_controls[0]); ++i) {
        bool rejected = false;
        try {
            rtsp::resolve_control_url(origin + "/live/", bad_controls[i]);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "unsupported control URL was accepted");
    }
}

void test_sdp() {
    // 手工给出不同于服务器默认值的 SDP，避免只测“自己生成自己解析”。
    const std::string external =
        "v=0\r\no=- 11 1 IN IP4 127.0.0.1\r\ns=Example\r\nt=0 0\r\n"
        "a=control:rtsp://127.0.0.1:8554/live\r\n"
        "m=video 0 RTP/AVP 104\r\na=rtpmap:104 H264/90000\r\n"
        "a=fmtp:104 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=control:/live/trackID=7\r\n";
    const rtsp::SessionDescription parsed = rtsp::parse_sdp(external);
    check(parsed.tracks[0].payload_type == 104 && parsed.tracks[0].encoding == "H264"
              && parsed.tracks[0].clock_rate == 90000, "SDP mapping was lost");
    check(parsed.tracks[0].control == "/live/trackID=7", "SDP media control was lost");
    check(parsed.control == "rtsp://127.0.0.1:8554/live", "SDP aggregate control was lost");
    check(parsed.tracks[0].fmtp == "packetization-mode=1;profile-level-id=42e01f", "fmtp was lost");
    const rtsp::SessionDescription roundtrip = rtsp::parse_sdp(rtsp::make_sdp(parsed));
    check(roundtrip.tracks[0].control == parsed.tracks[0].control, "SDP round trip failed");

    // 缺失 control/rtpmap 或出现不完整媒体，不能靠结构体默认值假装成功。
    const std::string bad[] = {
        "", "v=1\nm=video 0 RTP/AVP 96\na=rtpmap:96 H264/90000\na=control:trackID=0\n",
        "v=0\nm=video 0 RTP/AVP 96\na=control:trackID=0\n",
        "v=0\nm=video 0 RTP/AVP 96\na=rtpmap:96 H264/90000\n",
        "v=0\nm=video 0 RTP/AVP 999999999999\n",
        "v=0\nm=video 0 RTP/AVP 96 97\n",
        "v=0\nm=audio 0 RTP/AVP 96\n",
        "v=0\nm=video 0 RTP/AVP 96\na=rtpmap:97 H264/90000\na=control:trackID=0\n",
        "v=0\nm=video 0 RTP/AVP 96\na=rtpmap:96 H265/90000\na=control:trackID=0\n",
        external + "m=video 0 RTP/AVP 105\r\n",
        external + "a=control:duplicate\r\n"
    };
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        bool rejected = false;
        try {
            rtsp::parse_sdp(bad[i]);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "invalid or unsupported SDP was accepted");
    }
}

void test_complete_session() {
    rtsp::RtspServerSession session("12345");
    const std::string url = "rtsp://127.0.0.1:8554/live";
    rtsp::RtspMessage response = respond(session, request("OPTIONS", "*"));
    check(response.status_code == 200
              && response.header("Public") == "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN",
          "OPTIONS did not advertise the implemented methods");
    response = respond(session, request("DESCRIBE", url));
    check(response.status_code == 200 && response.header("Content-Type") == "application/sdp",
          "DESCRIBE did not return SDP");
    const rtsp::SessionDescription sdp = rtsp::parse_sdp(response.body);
    const std::string base = response.header("Content-Base");
    const std::string track_url = rtsp::resolve_control_url(base, sdp.tracks[0].control);
    const std::string aggregate_url = rtsp::resolve_control_url(base, sdp.control);
    check(track_url == url + "/trackID=0", "DESCRIBE returned an incorrect base URL");

    rtsp::RtspMessage setup = request("SETUP", track_url);
    setup.set_header("Transport", "RTP/AVP/TCP;unicast;interleaved=10-11");
    response = respond(session, setup);
    check(response.status_code == 200 && response.header("Session") == "12345",
          "SETUP did not establish a Session");
    check(response.header("Transport") == "RTP/AVP/TCP;unicast;interleaved=10-11"
              && session.tracks()[0].transport.rtp_channel == 10, "SETUP did not negotiate channels");
    check(session.state() == rtsp::SessionReady, "SETUP did not enter Ready");
    response = respond(session, request("PLAY", aggregate_url, "12345;timeout=60"));
    check(response.status_code == 200 && session.state() == rtsp::SessionPlaying,
          "PLAY did not enter Playing");
    response = respond(session, request("TEARDOWN", aggregate_url, "12345"));
    check(response.status_code == 200 && session.state() == rtsp::SessionClosed,
          "TEARDOWN did not close the session");
    check(respond(session, request("PLAY", aggregate_url, "12345")).status_code == 454,
          "closed Session was reused");
}

void test_multitrack_sdp_and_setup_order() {
    // 手工 SDP，音频在前，视频使用非默认 control。
    const std::string text = "v=0\r\na=control:*\r\n"
        "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 MPEG4-GENERIC/44100/1\r\n"
        "a=fmtp:97 mode=AAC-hbr;config=1208;SizeLength=13;IndexLength=3;IndexDeltaLength=3\r\n"
        "a=control:trackID=3\r\n"
        "m=video 0 RTP/AVP 104\r\na=rtpmap:104 H264/90000\r\na=control:trackID=7\r\n";
    const auto description = rtsp::parse_sdp(text);
    const auto roundtrip = rtsp::parse_sdp(rtsp::make_sdp(description));
    check(roundtrip.tracks.size() == 2 && roundtrip.tracks[0].channels == 1
              && roundtrip.tracks[0].clock_rate == 44100
              && roundtrip.tracks[1].control == "trackID=7", "multitrack SDP did not round trip");
    rtsp::RtspServerSession session("123", description);
    const std::string url = "rtsp://127.0.0.1/live";
    auto audio = request("SETUP", url + "/trackID=3");
    audio.set_header("Transport", "RTP/AVP/TCP;unicast;interleaved=10-11");
    check(respond(session, audio).status_code == 200, "audio-first SETUP failed");
    auto video = request("SETUP", url + "/trackID=7", "123");
    video.set_header("Transport", "RTP/AVP/TCP;unicast;interleaved=6-7");
    check(respond(session, video).status_code == 200, "custom video control was not resolved");
    check(session.has_channel(6) && session.has_channel(7) && session.has_channel(10)
              && session.has_channel(11) && !session.has_channel(0), "channel lookup mixed tracks");
    check(respond(session, request("PLAY", url, "123")).status_code == 200,
          "audio-first session could not PLAY");

    const std::string bad[] = {
        text + "a=rtpmap:104 H264/90000\r\n",
        text + "a=fmtp:97 wrong-track\r\n",
        "v=0\nm=audio 0 RTP/AVP 97\na=rtpmap:97 MPEG4-GENERIC/0/2\na=fmtp:97 config=1190\na=control:t\n",
        "v=0\nm=audio 0 RTP/AVP 97\na=rtpmap:97 MPEG4-GENERIC/48000/2\na=control:t\n"
    };
    for (const std::string& value : bad) {
        bool rejected = false;
        try {
            rtsp::parse_sdp(value);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "invalid audio SDP was accepted");
    }
}

void test_rejected_requests() {
    rtsp::RtspServerSession session("98765");
    const std::string url = "rtsp://127.0.0.1:8554/live";
    const std::string track = url + "/trackID=0";
    check(respond(session, request("PLAY", url)).status_code == 454, "PLAY accepted no Session");
    check(respond(session, request("PLAY", url, "98765")).status_code == 454,
          "PLAY succeeded before SETUP");
    check(respond(session, request("DESCRIBE", url + "missing")).status_code == 404,
          "unknown resource was accepted");
    check(respond(session, request("PAUSE", url)).status_code == 501, "unsupported method succeeded");
    check(respond(session, request("SETUP", url)).status_code == 459, "aggregate SETUP succeeded");
    rtsp::RtspMessage setup = request("SETUP", track);
    setup.set_header("Transport", "RTP/AVP;unicast;client_port=5000-5001");
    check(respond(session, setup).status_code == 461, "UDP Transport was accepted");
    check(session.state() == rtsp::SessionInit, "failed SETUP changed state");
    setup.set_header("Transport", "RTP/AVP/TCP;unicast;interleaved=2-3");
    check(respond(session, setup).status_code == 200, "valid SETUP failed after rejected SETUP");
    check(respond(session, setup).status_code == 455, "repeated SETUP was accepted");
    check(respond(session, request("PLAY", url, "wrong-id")).status_code == 454,
          "wrong Session was accepted");
    check(respond(session, request("TEARDOWN", url, "wrong-id")).status_code == 454,
          "wrong Session closed the connection");
    check(respond(session, request("PLAY", track, "98765")).status_code == 460,
          "track-level PLAY was accepted");
    rtsp::RtspMessage play = request("PLAY", url, "98765");
    play.set_header("Range", "npt=10-");
    check(respond(session, play).status_code == 457, "unsupported seek was accepted");
    check(session.state() == rtsp::SessionReady, "rejected request changed Ready state");
    play.set_header("Range", "npt=0.001-");
    check(respond(session, play).status_code == 457, "nonzero fractional seek was accepted");

    rtsp::RtspMessage duplicate = request("PLAY", url, "98765");
    rtsp::RtspHeader header;
    header.name = "session";
    header.value = "wrong-id";
    duplicate.headers.push_back(header);
    check(respond(session, duplicate).status_code == 400, "duplicate Session was accepted");
    play.set_header("Range", "npt=0.000-");
    check(respond(session, play).status_code == 200,
          "valid PLAY failed after rejected requests");
    check(respond(session, request("PLAY", url, "98765")).status_code == 455,
          "repeated PLAY was accepted");
    check(respond(session, request("TEARDOWN", url, "98765")).status_code == 200,
          "valid TEARDOWN failed");

    // 已 SETUP 但还没有 PLAY 的连接也可以正常结束。
    rtsp::RtspServerSession ready_session("11111");
    check(respond(ready_session, setup).status_code == 200, "second session SETUP failed");
    check(respond(ready_session, request("TEARDOWN", url, "11111")).status_code == 200,
          "TEARDOWN from Ready failed");
}

}  // namespace

int main() {
    try {
        test_transport_and_urls();
        std::cout << "PASS: TCP Transport, Session header and control URLs\n";
        test_sdp();
        std::cout << "PASS: SDP configuration and invalid input\n";
        test_complete_session();
        test_multitrack_sdp_and_setup_order();
        std::cout << "PASS: complete RTSP session with negotiated channels\n";
        test_rejected_requests();
        std::cout << "PASS: invalid Session, Transport, URL and state transitions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
