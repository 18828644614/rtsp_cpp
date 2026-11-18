#include "media/audio_frame_queue.h"
#include "media/media_time.h"

#include <iostream>
#include <limits>

namespace {
void check(bool ok) { if (!ok) { throw std::runtime_error("audio queue or signed time mismatch"); } }
rtsp::AacAccessUnit frame(std::int64_t pts, std::size_t bytes = 4) {
    return {std::string(bytes, 'a'), pts, 1024};
}
}

int main() {
    try {
        rtsp::AudioFrameQueue queue({2, 8});
        rtsp::AacAccessUnit out;
        check(queue.read(out) == rtsp::AudioFrameRead::Waiting);
        check(queue.push(frame(-1024)) && queue.push(frame(0)) && queue.push(frame(1024)));
        check(queue.stats().frames == 2 && queue.stats().bytes == 8 && queue.stats().dropped == 1);
        check(queue.read(out) == rtsp::AudioFrameRead::FrameReady && out.pts == 0);
        queue.finish();
        check(queue.read(out) == rtsp::AudioFrameRead::FrameReady && out.pts == 1024);
        check(queue.read(out) == rtsp::AudioFrameRead::Ended && out.data.empty());
        check(!queue.push(frame(2048)));

        rtsp::AudioFrameQueue bytes({8, 8});
        bytes.push(frame(0, 5));
        bytes.push(frame(1024, 5));
        check(bytes.stats().frames == 1 && bytes.stats().dropped == 1);
        bytes.push(frame(2048, 3));
        bytes.resume_at_latest();
        check(bytes.read(out) == rtsp::AudioFrameRead::FrameReady && out.pts == 2048);

        rtsp::AudioFrameQueue failed;
        failed.push(frame(-1024));
        failed.finish(std::make_exception_ptr(std::runtime_error("input failed")));
        check(failed.read(out) == rtsp::AudioFrameRead::FrameReady && out.pts == -1024);
        bool raised = false;
        try { failed.read(out); } catch (const std::runtime_error&) { raised = true; }
        check(raised);
        failed.discard_and_finish();
        check(failed.read(out) == rtsp::AudioFrameRead::Ended);

        check(rtsp::rescale_media_pts(-1024, 48000, 1000000) == -21333);
        check(rtsp::rescale_media_pts(-48000, 48000, 90000) == -90000);
        check(rtsp::media_pts_less(-1024, 48000, 0, 90000));
        check(rtsp::media_pts_less(-90001, 90000, -48000, 48000));
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        check(rtsp::rescale_media_pts(maximum, 48000, 48000) == maximum);
        check(rtsp::media_pts_less(maximum - 1, 48000, maximum, 48000));
        std::cout << "PASS: bounded audio queue, preserved PTS, EOF/error draining and signed time\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
