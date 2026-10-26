# H.264 与 AAC 测试样例

服务器默认读取本目录的 `sample.h264`。媒体文件不纳入 Git，生成后可直接用于本机测试。

输入需要是 H.264 Annex-B 文件，每个访问单元带 AUD，开头包含 SPS/PPS。按固定 25 fps 发送；允许含 B 帧，不解析真实 PTS/DTS。

多个客户端共享播放进度。晚加入者需要等待后续 IDR，建议每秒一个 IDR；下面的 `-g 25` 满足这一要求。文件播完后需要重启服务器才能从头播放。

在项目根目录执行：

```powershell
ffmpeg -f lavfi -i "testsrc2=size=320x180:rate=25" -t 5 -c:v libx264 -pix_fmt yuv420p -bf 2 -g 25 -x264-params "aud=1:repeat-headers=1" -f h264 samples/sample.h264
```

如果已有 MP4 文件，可重新编码成满足约定的样例：

```powershell
ffmpeg -i input.mp4 -an -vf fps=25 -c:v libx264 -pix_fmt yuv420p -bf 2 -g 25 -x264-params "aud=1:repeat-headers=1" -f h264 samples/sample.h264
```

FFmpeg 仅用于准备样例和验收，不参与 C++ 程序构建。已有合适文件时不必安装 FFmpeg 才能运行客户端和服务器。

## 双轨道样例

音视频测试使用固定 25 fps、无 B 帧的视频和 AAC-LC 音频，采样率与声道数保持不变。下面两条命令各生成约 30 秒文件，均视为从媒体时间零开始：

```powershell
ffmpeg -f lavfi -i "testsrc2=size=320x180:rate=25" -t 30 -c:v libx264 -pix_fmt yuv420p -bf 0 -g 25 -x264-params "aud=1:repeat-headers=1" -f h264 samples/sample.h264
ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 30 -ac 2 -c:a aac -profile:a aac_low -b:a 128k -f adts samples/sample.aac

.\build\Debug\rtsp_server.exe 8554 127.0.0.1 samples/sample.h264 samples/sample.aac
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

AAC 编码可能使时长比视频略长；服务器会继续发送尚未结束的轨道，全部结束后关闭订阅。两种裸流不包含容器中的起始偏移，不会自动恢复原 MP4 的时间线。
