#pragma once

#include "rtsp/rtsp_message.h"
#include "rtsp/rtsp_session_utils.h"
#include "sdp/session_description.h"

namespace rtsp {

enum RtspSessionState {
    SessionInit,     // 尚未 SETUP。
    SessionReady,    // SETUP 成功，等待 PLAY。
    SessionPlaying,
    SessionClosed    // TEARDOWN 成功，应关闭连接。
};

struct TrackState {
    bool setup = false;
    TcpTransport transport;
};

// 只判断请求、生成响应和保存状态，不直接收发 Socket。
// 这样可以直接用固定请求测试错误流程，不必为每个测试建立连接。
class RtspServerSession {
public:
    explicit RtspServerSession(const std::string& session_id,
                               const SessionDescription& description = SessionDescription(),
                               unsigned int timeout_seconds = 60);

    //根据客户端的请求和当前会话状态，决定是否允许操作，更新会话，并生成对应的 RTSP 响应
    RtspMessage handle(const RtspMessage& request);
    RtspSessionState state() const;
    const std::vector<TrackState>& tracks() const;
    bool has_channel(unsigned int channel) const;

private:
    std::string session_id_;
    unsigned int timeout_seconds_ = 60;
    RtspSessionState state_ = SessionInit;
    std::vector<TrackState> tracks_;
    SessionDescription description_;
};

}  // namespace rtsp
