#pragma once

#include <chrono>
#include <cstdint>

namespace rtsp {

// 调度和超时使用单调时钟；墙钟仅在建立映射时采样一次。
class MediaClock {
public:
    using Clock = std::chrono::steady_clock;
    MediaClock(Clock::time_point monotonic = Clock::now(),
               std::chrono::system_clock::time_point wall = std::chrono::system_clock::now());
    std::uint64_t ntp_at(Clock::time_point now) const;
    static std::uint32_t rtp_at(std::uint32_t timestamp, Clock::time_point sampled_at,
                                int clock_rate, Clock::time_point now);

private:
    Clock::time_point monotonic_;
    std::chrono::system_clock::time_point wall_;
};

}  // namespace rtsp
