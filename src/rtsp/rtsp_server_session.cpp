#include "rtsp/rtsp_server_session.h"
#include "sdp/session_description.h"

#include <stdexcept>

namespace rtsp {
namespace {

RtspMessage response_for(const RtspMessage& request, int code, const std::string& reason) {
    RtspMessage response;
    response.is_request = false;
    response.status_code = code;
    response.reason = reason;
    response.set_header("CSeq", request.header("CSeq"));
    return response;
}

//拒绝同时出现多个字段
bool duplicate_header(const RtspMessage& request, const std::string& name) {
    // 参数 name 使用小写；把实际字段名也转成小写后比较。
    int count = 0;
    for (std::size_t i = 0; i < request.headers.size(); ++i) {
        std::string field_name = request.headers[i].name;
        for (std::size_t j = 0; j < field_name.size(); ++j) {
            if (field_name[j] >= 'A' && field_name[j] <= 'Z') {
                field_name[j] = static_cast<char>(field_name[j] - 'A' + 'a');
            }
        }
        if (field_name == name) {
            ++count;
        }
    }
    return count > 1;
}

// FFmpeg 使用 npt=0.000-；它与 npt=0- 都表示从零开始。
bool is_zero_start_range(const std::string& range) {
    if (range == "npt=0-") {
        return true;
    }
    if (range.size() < 8 || range.compare(0, 6, "npt=0.") != 0 || range.back() != '-') {
        return false;
    }
    return range.substr(6, range.size() - 7).find_first_not_of('0') == std::string::npos;
}

}  // namespace

RtspServerSession::RtspServerSession(const std::string& session_id,
                                   const SessionDescription& description) {
    if (session_id.empty() || session_id.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("this demo uses a nonempty numeric session id");
    }
    session_id_ = session_id;
    description_ = description;
    tracks_.resize(description_.tracks.size());
}

RtspMessage RtspServerSession::handle(const RtspMessage& request) {
    //先拒绝错误的消息和重复头字段
    if (!request.is_request || duplicate_header(request, "session")
        || duplicate_header(request, "transport")) {
        return response_for(request, 400, "Bad Request");
    }
    if (state_ == SessionClosed) {
        return response_for(request, 454, "Session Not Found");
    }

    const std::string method = request.method;
    if (method != "OPTIONS" && method != "DESCRIBE" && method != "SETUP"
        && method != "PLAY" && method != "TEARDOWN") {
        return response_for(request, 501, "Not Implemented");
    }

    std::string path;
    if (method == "OPTIONS" && request.uri == "*") {
        path = "*";
    } else {
        try {
            //去除资源路径：
            //例如：
            // rtsp://127.0.0.1:8554/live/trackID=0
            // └────── origin ─────┘└---─ path --─┘
            // path   = "/live/trackID=0"
            path = rtsp_url_path(request.uri);
        } catch (const std::runtime_error&) {
            return response_for(request, 400, "Bad Request");
        }
    }

    // 资源路径仍为 /live，轨道路径来自 SDP 中的 control。
    const bool is_aggregate = path == "/live" || path == "/live/";
    int track_index = -1;
    for (std::size_t i = 0; i < description_.tracks.size(); ++i) {
        if (path == "*") {
            break;
        }
        const std::string control_url = resolve_control_url(
            request.uri.substr(0, request.uri.find('/', 7)) + "/live/",
            description_.tracks[i].control);
        if (path == rtsp_url_path(control_url)) {
            track_index = static_cast<int>(i);
            break;
        }
    }
    const bool is_track = track_index >= 0;
    if (!is_aggregate && !is_track && path != "*") {
        return response_for(request, 404, "Not Found");
    }

    // 已携带 Session 时必须匹配；PLAY 和 TEARDOWN 还要求不能缺失。
    const std::string requested_session = request.header("Session");
    if (!requested_session.empty()
        && (state_ == SessionInit || session_id_from_header(requested_session) != session_id_)) {
        return response_for(request, 454, "Session Not Found");
    }
    if ((method == "PLAY" || method == "TEARDOWN") && requested_session.empty()) {
        return response_for(request, 454, "Session Not Found");
    }
    
    //默认创建成功响应
    RtspMessage response = response_for(request, 200, "OK");
    if (method == "OPTIONS") {
        response.set_header("Public", "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN");
    } else if (method == "DESCRIBE") {
        if (!is_aggregate) {
            return response_for(request, 460, "Only Aggregate Operation Allowed");
        }
        const std::string accept = request.header("Accept");
        if (!accept.empty() && accept != "application/sdp" && accept != "*/*") {
            return response_for(request, 406, "Not Acceptable");
        }
        // 以客户端请求中的主机和端口作基地址，不把 0.0.0.0 广播给客户端。
        std::string base = request.uri;
        if (base[base.size() - 1] != '/') {
            base += '/';
        }
        response.set_header("Content-Type", "application/sdp");
        response.set_header("Content-Base", base);
        response.body = make_sdp(description_);
    } else if (method == "SETUP") {
        if (!is_track) {
            return response_for(request, 459, "Aggregate Operation Not Allowed");
        }
        if ((state_ != SessionInit && state_ != SessionReady) || tracks_[track_index].setup) {
            return response_for(request, 455, "Method Not Valid in This State");
        }
        if (state_ == SessionReady && requested_session.empty()) {
            return response_for(request, 454, "Session Not Found");
        }
        TcpTransport requested_transport;
        if (!parse_tcp_transport(request.header("Transport"), requested_transport)) {
            return response_for(request, 461, "Unsupported Transport");
        }
        // 只有全部检查通过，才保存通道并切换状态；错误请求不改变会话。
        if (has_channel(requested_transport.rtp_channel)
            || has_channel(requested_transport.rtcp_channel)) {
            return response_for(request, 461, "Unsupported Transport");
        }
        tracks_[track_index].setup = true;
        tracks_[track_index].transport = requested_transport;
        state_ = SessionReady;
        response.set_header("Transport", make_tcp_transport(requested_transport));
        response.set_header("Session", session_id_);
    } else if (method == "PLAY") {
        if (!is_aggregate) {
            return response_for(request, 460, "Only Aggregate Operation Allowed");
        }
        if (state_ != SessionReady) {
            return response_for(request, 455, "Method Not Valid in This State");
        }
        // 暂不支持任意位置 seek；仅接受从头播放，或不提供 Range。
        if (!request.header("Range").empty() && !is_zero_start_range(request.header("Range"))) {
            return response_for(request, 457, "Invalid Range");
        }
        state_ = SessionPlaying;
        response.set_header("Session", session_id_);
    } else if (method == "TEARDOWN") {
        if (!is_aggregate) {
            return response_for(request, 460, "Only Aggregate Operation Allowed");
        }
        if (state_ != SessionReady && state_ != SessionPlaying) {
            return response_for(request, 455, "Method Not Valid in This State");
        }
        state_ = SessionClosed;
        response.set_header("Session", session_id_);
    }
    return response;
}

RtspSessionState RtspServerSession::state() const {
    return state_;
}

const std::vector<TrackState>& RtspServerSession::tracks() const {
    return tracks_;
}

bool RtspServerSession::has_channel(unsigned int channel) const {
    for (const TrackState& track : tracks_) {
        if (track.setup && (channel == static_cast<unsigned int>(track.transport.rtp_channel)
            || channel == static_cast<unsigned int>(track.transport.rtcp_channel))) {
            return true;
        }
    }
    return false;
}

}  // namespace rtsp
