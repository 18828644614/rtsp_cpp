#pragma once

#include <chrono>

namespace rtsp {

// 所有期限由同一个事件循环检查；测试可缩短报告周期和会话期限。
struct RuntimeOptions {
    unsigned int session_timeout_seconds = 60;
    std::chrono::milliseconds rtcp_interval{5000};
    std::chrono::milliseconds control_idle_timeout{60000};
    std::chrono::milliseconds send_stall_timeout{2000};
    std::chrono::milliseconds drain_timeout{2000};
};

}  // namespace rtsp
