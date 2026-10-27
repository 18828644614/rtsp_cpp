#include "media/media_clock.h"

#include <stdexcept>

namespace rtsp {

MediaClock::MediaClock(Clock::time_point monotonic, std::chrono::system_clock::time_point wall)
    : monotonic_(monotonic), wall_(wall) {}

std::uint64_t MediaClock::ntp_at(Clock::time_point now) const {
    const auto elapsed = wall_.time_since_epoch() + (now - monotonic_);
    const auto seconds = std::chrono::floor<std::chrono::seconds>(elapsed);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed - seconds).count();
    const auto ntp_seconds = static_cast<std::uint64_t>(seconds.count() + 2208988800LL);
    return (ntp_seconds << 32) | (static_cast<std::uint64_t>(nanos) * (1ULL << 32) / 1000000000ULL);
}

std::uint32_t MediaClock::rtp_at(std::uint32_t timestamp, Clock::time_point sampled_at,
                                int clock_rate, Clock::time_point now) {
    if (clock_rate <= 0) {
        throw std::invalid_argument("RTP clock rate must be positive");
    }
    const auto elapsed = now - sampled_at;
    const auto seconds = std::chrono::floor<std::chrono::seconds>(elapsed);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed - seconds).count();
    // 先拆秒，避免长时间运行时纳秒乘采样率溢出。uint32 自然实现 RTP 回绕。
    return timestamp + static_cast<std::uint32_t>(seconds.count() * clock_rate)
           + static_cast<std::uint32_t>(nanos * clock_rate / 1000000000LL);
}

}  // namespace rtsp
