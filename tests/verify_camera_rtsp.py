"""可选的独立 FFmpeg 网络验收；生成帧来自测试程序，不需要摄像头。"""

import argparse
import json
import re
import subprocess
import time
from pathlib import Path


def wait_for(predicate, deadline, message):
    while not predicate():
        if time.monotonic() >= deadline:
            raise RuntimeError(message)
        time.sleep(0.02)


def frame_hashes(path):
    return [line.rsplit(",", 1)[1].strip()
            for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    processes = []
    handles = []
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        server_log = output / "server.log"
        handles.append(server_log.open("wb"))
        server = subprocess.Popen([str(args.fixture.resolve()), "--serve"],
                                  cwd=output, stdout=handles[-1], stderr=subprocess.STDOUT,
                                  creationflags=flags)
        processes.append(server)

        def log():
            return server_log.read_text(encoding="utf-8", errors="replace")

        wait_for(lambda: "camera_test_url=" in log() or server.poll() is not None,
                 time.monotonic() + 5, "test server did not publish its URL")
        match = re.search(r"camera_test_url=(rtsp://\S+)", log())
        if not match:
            raise RuntimeError(log())
        url = match.group(1)

        def receiver(name, frames):
            handles.append((output / f"{name}.log").open("wb"))
            process = subprocess.Popen([
                str(args.ffmpeg.resolve()), "-hide_banner", "-loglevel", "error", "-y",
                "-rtsp_transport", "tcp", "-analyzeduration", "0", "-probesize", "32",
                "-i", url, "-map", "0:v:0", "-frames:v", str(frames),
                "-fps_mode", "passthrough", "-f", "framemd5", str(output / f"{name}.md5")],
                cwd=output, stdout=handles[-1], stderr=subprocess.STDOUT, creationflags=flags)
            processes.append(process)
            return process

        first = receiver("a", 90)
        wait_for(lambda: "request: PLAY" in log() or first.poll() is not None,
                 time.monotonic() + 4, "first FFmpeg did not PLAY")
        time.sleep(0.4)
        second = receiver("b", 30)
        for process in (second, first):
            if process.wait(timeout=8) != 0:
                raise RuntimeError("FFmpeg failed; inspect receiver logs")
        a = frame_hashes(output / "a.md5")
        b = frame_hashes(output / "b.md5")
        if len(a) != 90 or len(b) != 30:
            raise RuntimeError(f"unexpected frame counts: a={len(a)}, b={len(b)}")
        offsets = [i for i in range(1, len(a) - len(b) + 1) if a[i:i + len(b)] == b]
        if not offsets:
            raise RuntimeError("late FFmpeg decoded different frames")
        if any((output / f"{name}.log").read_text(encoding="utf-8").strip() for name in ("a", "b")):
            raise RuntimeError("FFmpeg reported decode errors; inspect receiver logs")
        if server.wait(timeout=12) != 0:
            raise RuntimeError("test server failed during shutdown")
        result = {"first_frames": len(a), "late_frames": len(b), "late_offset": offsets[0],
                  "decoded_hashes_match": True, "ffmpeg_errors": False}
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result))
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
        for handle in handles:
            handle.close()


if __name__ == "__main__":
    main()
