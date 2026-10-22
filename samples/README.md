# H.264 测试样例

服务器默认读取本目录的 `sample.h264`。媒体文件不纳入 Git，生成后可直接用于本机测试。

输入需要是 H.264 Annex-B 文件，每个访问单元带 AUD，开头包含 SPS/PPS。按固定 25 fps 发送；允许含 B 帧，不解析真实 PTS/DTS。

在项目根目录执行：

```powershell
ffmpeg -f lavfi -i "testsrc2=size=320x180:rate=25" -t 5 -c:v libx264 -pix_fmt yuv420p -bf 2 -g 25 -x264-params "aud=1:repeat-headers=1" -f h264 samples/sample.h264
```

如果已有 MP4 文件，可重新编码成满足约定的样例：

```powershell
ffmpeg -i input.mp4 -an -vf fps=25 -c:v libx264 -pix_fmt yuv420p -bf 2 -g 25 -x264-params "aud=1:repeat-headers=1" -f h264 samples/sample.h264
```

FFmpeg 仅用于准备样例和验收，不参与 C++ 程序构建。已有合适文件时不必安装 FFmpeg 才能运行客户端和服务器。
