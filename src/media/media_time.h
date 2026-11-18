#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace rtsp {

inline std::int64_t rescale_media_pts(std::int64_t pts, int from_rate, int to_rate) {
    if (from_rate <= 0 || to_rate <= 0) { throw std::invalid_argument("invalid media clock rate"); }
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto seconds = pts / from_rate;
    if (seconds > maximum / to_rate || seconds < minimum / to_rate) {
        throw std::overflow_error("media timestamp out of range");
    }
    const auto whole = seconds * to_rate;
    const auto fraction = (pts % from_rate) * to_rate / from_rate;
    if ((fraction > 0 && whole > maximum - fraction) ||
        (fraction < 0 && whole < minimum - fraction)) {
        throw std::overflow_error("media timestamp out of range");
    }
    return whole + fraction;
}

inline bool media_pts_less(std::int64_t first, int first_rate,
                            std::int64_t second, int second_rate) {
    if (first_rate <= 0 || second_rate <= 0) { throw std::invalid_argument("invalid media clock rate"); }
    if (first / first_rate != second / second_rate) {
        return first / first_rate < second / second_rate;
    }
    return (first % first_rate) * second_rate < (second % second_rate) * first_rate;
}

}
