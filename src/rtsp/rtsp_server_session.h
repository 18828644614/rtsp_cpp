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

// 只判断请求、生成响应和保存状态，不直接收发 Socket。
// 这样可以直接用固定请求测试错误流程，不必为每个测试建立连接。
class RtspServerSession {
public:
    explicit RtspServerSession(const std::string& session_id,
                               const SessionDescription& description = SessionDescription());

    RtspMessage handle(const RtspMessage& request);
    RtspSessionState state() const;
    TcpTransport transport() const;

private:
    std::string session_id_;
    RtspSessionState state_ = SessionInit;
    TcpTransport transport_;
    SessionDescription description_;
};

}  // namespace rtsp
