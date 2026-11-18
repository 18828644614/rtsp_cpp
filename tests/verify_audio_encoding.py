"""Verify AAC padding, decoded pulse, and external pure-audio RTSP reception."""

import argparse
import array
import csv
import json
from pathlib import Path
import socket
import subprocess
import sys
import time


def run(command, output):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=15, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    output.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"command failed; inspect {output}: {result.stderr.decode(errors='replace')}")
    return result


def floats(path):
    samples = array.array("f")
    samples.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        samples.byteswap()
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    encoded = output / "probe.aac"
    run([str(args.probe.resolve()), "--synthetic", "--seconds", "1", "--output", str(encoded)],
        output / "probe.log")
    stats = json.loads(Path(str(encoded) + ".json").read_text(encoding="utf-8"))
    with Path(str(encoded) + ".csv").open(encoding="utf-8", newline="") as trace:
        packets = [row for row in csv.DictReader(trace) if row["kind"] == "aac"]
    for index, packet in enumerate(packets):
        if int(packet["pts"]) != -stats["initial_padding"] + index * 1024 or int(packet["samples"]) != 1024:
            raise RuntimeError("AAC packet PTS/duration is discontinuous")
    if stats["input_samples"] != 44117 or stats["resampled_samples"] != 48019:
        raise RuntimeError("unexpected resampling count")
    if stats["submitted_samples"] != stats["resampled_samples"] + stats["tail_padding"]:
        raise RuntimeError("FIFO tail sample accounting mismatch")
    decoded = output / "probe.f32"
    decode = [str(args.ffmpeg.resolve()), "-hide_banner", "-v", "error", "-y"]
    run(decode + ["-i", str(encoded), "-map", "0:a:0", "-c:a", "pcm_f32le", "-f", "f32le", str(decoded)],
        output / "decode.log")
    samples = floats(decoded)
    if len(samples) != stats["submitted_samples"] + stats["initial_padding"]:
        raise RuntimeError("external decoder sample count disagrees with initial/tail padding")
    peak = max(range(len(samples)), key=lambda position: abs(samples[position]))
    if abs(peak - stats["initial_padding"] - 12000) > 24 or abs(samples[peak]) < 0.1:
        raise RuntimeError("external decoded pulse position mismatch")

    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    server_log = output / "server.log"
    with server_log.open("wb") as log:
        server = subprocess.Popen([str(args.server.resolve()), "--synthetic-audio", "--port", str(port)],
                                  stdout=log, stderr=subprocess.STDOUT,
                                  creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        try:
            deadline = time.monotonic() + 5
            while "RTSP server listening" not in server_log.read_text(encoding="utf-8", errors="replace"):
                if server.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("audio server failed to start; inspect server.log")
                time.sleep(0.02)
            receive = decode + ["-rtsp_transport", "tcp", "-analyzeduration", "0", "-probesize", "32",
                                "-i", f"rtsp://127.0.0.1:{port}/live", "-map", "0:a:0", "-frames:a", "48"]
            crc_file = output / "rtsp.framecrc"
            run(receive + ["-c:a", "copy", "-f", "framecrc", str(crc_file)], output / "rtsp-copy.log")
            rows = [line.split(",") for line in crc_file.read_text().splitlines()
                    if line and not line.startswith("#")]
            pts = [int(row[2]) for row in rows]
            if len(pts) != 48 or any(after - before != 1024 for before, after in zip(pts, pts[1:])):
                raise RuntimeError("external RTSP packet timestamps are not continuous")
            network_pcm = output / "rtsp.f32"
            run(receive + ["-c:a", "pcm_f32le", "-f", "f32le", str(network_pcm)], output / "rtsp-decode.log")
            network_samples = floats(network_pcm)
            if len(network_samples) != 48 * 1024 or max(map(abs, network_samples), default=0) < 0.01:
                raise RuntimeError("external RTSP decoder produced missing/silent PCM")
        finally:
            if server.poll() is None:
                server.terminate()
            server.wait(timeout=5)

    evidence = {**stats, "first_aac_pts": int(packets[0]["pts"]),
                "decoded_samples": len(samples), "decoded_pulse_position": peak,
                "expected_pulse_position": stats["initial_padding"] + 12000,
                "rtsp_packets": len(pts), "rtsp_pts_step": 1024,
                "rtsp_decoded_samples": len(network_samples)}
    (output / "result.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(evidence))


if __name__ == "__main__":
    main()
