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

}  // namespace

RtspServerSession::RtspServerSession(const std::string& session_id) {
    if (session_id.empty() || session_id.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("this demo uses a nonempty numeric session id");
    }
    session_id_ = session_id;
}

RtspMessage RtspServerSession::handle(const RtspMessage& request) {
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
            path = rtsp_url_path(request.uri);
        } catch (const std::runtime_error&) {
            return response_for(request, 400, "Bad Request");
        }
    }
    const bool is_aggregate = path == "/live" || path == "/live/";
    const bool is_track = path == "/live/trackID=0";
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
        SessionDescription description;
        response.set_header("Content-Type", "application/sdp");
        response.set_header("Content-Base", base);
        response.body = make_sdp(description);
    } else if (method == "SETUP") {
        if (!is_track) {
            return response_for(request, 459, "Aggregate Operation Not Allowed");
        }
        if (state_ != SessionInit) {
            return response_for(request, 455, "Method Not Valid in This State");
        }
        TcpTransport requested_transport;
        if (!parse_tcp_transport(request.header("Transport"), requested_transport)) {
            return response_for(request, 461, "Unsupported Transport");
        }
        // 只有全部检查通过，才保存通道并切换状态；错误请求不改变会话。
        transport_ = requested_transport;
        state_ = SessionReady;
        response.set_header("Transport", make_tcp_transport(transport_));
        response.set_header("Session", session_id_);
    } else if (method == "PLAY") {
        if (!is_aggregate) {
            return response_for(request, 460, "Only Aggregate Operation Allowed");
        }
        if (state_ != SessionReady) {
            return response_for(request, 455, "Method Not Valid in This State");
        }
        // 暂不支持任意位置 seek；仅接受从头播放，或不提供 Range。
        if (!request.header("Range").empty() && request.header("Range") != "npt=0-") {
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

TcpTransport RtspServerSession::transport() const {
    return transport_;
}

}  // namespace rtsp
