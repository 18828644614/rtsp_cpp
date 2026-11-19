#pragma once

#include <chrono>
#include <cstdint>

namespace rtsp {

class CaptureTimeline {
public:
    using Clock = std::chrono::steady_clock;

    explicit CaptureTimeline(Clock::time_point epoch = Clock::now()) : epoch_(epoch) {}

    std::int64_t relative_us(std::int64_t steady_us) const {
        return steady_us - std::chrono::duration_cast<std::chrono::microseconds>(
            epoch_.time_since_epoch()).count();
    }

    Clock::time_point at(std::chrono::microseconds offset) const { return epoch_ + offset; }

private:
    const Clock::time_point epoch_;
};

}  // namespace rtsp
