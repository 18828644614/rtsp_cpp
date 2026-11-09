#include "media/video_frame_queue.h"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool ok, const char* message) {
    if (!ok) { throw std::runtime_error(message); }
}
rtsp::H264AccessUnit frame(std::uint64_t pts, bool key = false, std::size_t bytes = 4) {
    return {{std::string(bytes, key ? '\x65' : '\x41')}, key, pts, 3000};
}
void test_limits_and_recovery() {
    rtsp::VideoFrameQueue queue({3, 12});
    rtsp::H264AccessUnit out = frame(99);
    check(queue.read(out) == rtsp::VideoFrameRead::Waiting && out.nalus.empty(), "empty is not Waiting");
    check(!queue.push(frame(0)), "initial P frame accepted");
    check(queue.push(frame(1, true)) && queue.push(frame(2)) && queue.push(frame(3)), "valid GOP rejected");
    check(!queue.push(frame(4)) && queue.stats().frames == 0
              && queue.stats().waiting_for_idr, "overflow retained broken references");
    check(!queue.push(frame(5)), "P frame accepted after overflow");
    check(queue.push(frame(6, true)) && queue.push(frame(7)), "IDR did not recover queue");
    check(queue.read(out) == rtsp::VideoFrameRead::FrameReady && out.pts == 6, "recovery missed IDR");
    check(queue.read(out) == rtsp::VideoFrameRead::FrameReady && out.pts == 7, "recovery lost P");
    check(queue.stats().bytes == 0 && queue.stats().dropped == 6, "queue accounting mismatch");

    rtsp::VideoFrameQueue bytes({100, 10});
    check(bytes.push(frame(0, true, 6)), "byte budget rejected IDR");
    check(!bytes.push(frame(1, false, 5)) && bytes.stats().frames == 0, "byte overflow not bounded");
    check(!bytes.push(frame(2, true, 11)), "oversize IDR accepted");
    check(bytes.push(frame(3, true, 10)), "exact byte limit rejected");
    check(bytes.push(frame(4, true, 9)) && bytes.stats().frames == 1, "new IDR did not replace overflow");
}
void test_resume_and_terminal_states() {
    rtsp::VideoFrameQueue queue({10, 100});
    queue.push(frame(0, true)); queue.push(frame(1));
    queue.push(frame(2, true)); queue.push(frame(3));
    queue.resume_at_latest_idr();
    rtsp::H264AccessUnit out;
    check(queue.stats().frames == 2 && queue.read(out) == rtsp::VideoFrameRead::FrameReady
              && out.pts == 2, "resume retained old GOP");
    queue.resume_at_latest_idr();
    check(queue.stats().frames == 0 && !queue.push(frame(4)), "resume started on P frame");
    queue.push(frame(5, true));
    queue.finish();
    check(!queue.push(frame(6, true)), "closed queue accepted frames");
    check(queue.read(out) == rtsp::VideoFrameRead::FrameReady && out.pts == 5, "EOF lost queued frame");
    check(queue.read(out) == rtsp::VideoFrameRead::Ended && out.nalus.empty(), "EOF not reported after drain");

    rtsp::VideoFrameQueue failed;
    failed.push(frame(0, true));
    failed.finish(std::make_exception_ptr(std::runtime_error("device lost")));
    check(failed.read(out) == rtsp::VideoFrameRead::FrameReady, "failure lost valid buffered frame");
    bool raised = false;
    try { failed.read(out); } catch (const std::runtime_error& e) { raised = std::string(e.what()) == "device lost"; }
    check(raised && out.nalus.empty(), "producer error not propagated");
    failed.discard_and_finish();
    check(failed.read(out) == rtsp::VideoFrameRead::Ended, "stop did not clear failure");
}
void test_concurrent_queue() {
    rtsp::VideoFrameQueue queue({8, 64});
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < 20000; ++i) { queue.push(frame(i, i % 7 == 0)); }
        queue.finish();
    });
    bool valid = true;
    bool first = true;
    std::uint64_t previous = 0;
    for (;;) {
        rtsp::H264AccessUnit out;
        const auto result = queue.read(out);
        const auto stats = queue.stats();
        valid = valid && stats.frames <= 8 && stats.bytes <= 64;
        if (result == rtsp::VideoFrameRead::Ended) { break; }
        if (result == rtsp::VideoFrameRead::Waiting) { std::this_thread::yield(); continue; }
        valid = valid && (first ? out.is_keyframe : out.pts > previous);
        if (!first && out.pts != previous + 1) { valid = valid && out.is_keyframe; }
        first = false;
        previous = out.pts;
    }
    producer.join();
    check(valid, "concurrent queue broke bounds, ordering or IDR recovery");
}
}

int main() {
    try {
        test_limits_and_recovery();
        test_resume_and_terminal_states();
        test_concurrent_queue();
        std::cout << "PASS: bounded queue, IDR recovery, resume, EOF, errors and concurrent access\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
