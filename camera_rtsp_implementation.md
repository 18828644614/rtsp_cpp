# 摄像头阶段 5：RTSP 输出实现说明

2026-11-09：服务器摄像头入口、VideoSource 注入、可选链接和真实 TCP 多客户端验证已完成。当前设备枚举为 `video_devices=0`，生成帧网络测试及独立 FFmpeg 解码已通过；真实摄像头、VLC/ffplay 画面和物理拔出仍待实机验收。

## 数据流和所有权

```text
server_main：解析参数，创建输入和视频源
    → CameraVideoSource：采集/编码工作线程 → VideoFrameQueue
    → RtspServer：接管 unique_ptr<VideoSource>
    → 一个共享 MediaSession：按真实编码 PTS 调度，AU 只封包一次
    → 每个客户端的 MediaSubscription / RtpSender / OutboundQueue
    → RtspServerConnection：事件循环非阻塞写入 RTSP/TCP
```

整个服务器只创建一个摄像头源和一个编码器。客户端各自有 Session、SSRC、序号、相对 RTP 时间和发送队列；慢连接的队列超限或发送停滞只终止该连接。生产者溢出恢复仍按阶段 4 的 IDR 规则处理。

文件模式保留原有路径构造函数；新增 `RtspServer(address, port, unique_ptr<VideoSource>, RuntimeOptions)`，协议层不包含摄像头或 FFmpeg 类型。单视频源注入入口不接受音频文件，摄像头首版仅视频。

## 修改位置

| 文件 | 职责 |
| --- | --- |
| `src/app/server_options.h` | 独立参数解析，保留文件位置参数，校验摄像头选择和数值范围 |
| `src/app/server_main.cpp` | 宽字符参数转 UTF-8、枚举设备、统一采集编码配置、创建源和服务器 |
| `src/rtsp/rtsp_server.h/.cpp` | 接管通用视频源，复用共享会话和原事件循环 |
| `src/media/camera_video_source.h/.cpp` | 首帧等待期间检查可选的外部停止标记 |
| `src/media/h264_file_reader.cpp`、`aac_file_reader.cpp` | 以 UTF-8 路径打开文件，匹配新的 Windows 宽字符入口 |
| `CMakeLists.txt`、`cmake/H264Encoding.cmake` | 按功能开关链接和复制 DLL，注册参数及网络测试 |
| `tests/camera_rtsp_tests.cpp` | 按帧率生成原始图像，走真实编码、服务器、TCP、RTP 重组和 H.264 解码 |
| `tests/verify_camera_rtsp.py` | 可选独立 FFmpeg 双客户端网络验收和逐帧 MD5 比较 |

## 构建

默认构建仍无需 FFmpeg，支持文件播放和参数检查。只开启编码可以运行全部无设备网络测试，真实设备入口还需要采集开关。只开启采集时可枚举设备，推流会明确提示缺少编码功能。

```powershell
cmake -S . -B build-camera-encoder `
    -DENABLE_CAMERA_CAPTURE=ON -DENABLE_H264_ENCODING=ON `
    -DFFMPEG_ROOT="E:/MarkDown/rtsp-cpp/.verify/stage3-sdk/ffmpeg-master-latest-win64-gpl-shared"
cmake --build build-camera-encoder --config Debug --parallel 4
ctest --test-dir build-camera-encoder -C Debug --output-on-failure
```

替换 SDK 路径为本机 FFmpeg shared 开发包，需包含 libx264、avdevice、avformat、avcodec、avutil、swscale。构建会把 SDK 的 DLL 复制到摄像头版服务器旁边；部署时保留这些 DLL。

## 启动和观看

```powershell
# 查看设备编号和名称（不打开设备）
.\build-camera-encoder\Debug\rtsp_server.exe --list-cameras

# 按编号选择，默认 1280x720、30 fps、2 Mbps、yuyv422、1 秒 GOP
.\build-camera-encoder\Debug\rtsp_server.exe --camera 0 --port 8554 --bind 0.0.0.0

# 按名称选择，可配置支持的采集参数
.\build-camera-encoder\Debug\rtsp_server.exe --device "Integrated Camera" `
    --port 8554 --bind 0.0.0.0 --width 1280 --height 720 `
    --fps 30 --bitrate 2000000 --gop 30 --pixel-format yuyv422

# 原文件模式
.\build\Debug\rtsp_server.exe 8554 127.0.0.1 samples/sample.h264

# 本机播放；局域网观看时换成服务器实际 IPv4 地址
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

VLC 打开相同地址，并在客户端启用 RTP over RTSP/TCP。`0.0.0.0` 仅用于监听，播放器应使用实际地址。程序默认监听 `127.0.0.1`，局域网服务需要显式使用 `--bind 0.0.0.0`。

`--camera INDEX` 与 `--device NAME` 二选一；`--list-cameras` 单独使用。分辨率必须为 2～16384 的偶数，帧率 1～240，码率为正整数，GOP 1～10000；未指定 GOP 时等于 fps。输入格式为 `yuyv422` 或 `bgr24`。设备必须支持请求的分辨率、帧率和原始格式，协商不匹配时报告失败，首版不自动降级或接收 MJPEG。

## 协议和生命周期

1. 构造源时启动工作线程，等待首个成功入队的 IDR 和真实 SPS/PPS，默认最多 5 秒；失败时回收线程，服务器不会开始监听。
2. `CameraVideoSource::track()` 的 fmtp 经 `MediaSession` 进入 DESCRIBE 响应，包含实际 profile-level-id 和 sprop-parameter-sets。
3. PLAY 成功响应先入队，媒体随后发送；每个订阅等待 IDR，并以自己的首个 IDR 为 RTP 时间零点，视频时钟为 90 kHz。
4. 第二个客户端加入共享进度，接受 FFmpeg 的 `Range: npt=0.000-`，不回放旧画面。IDR 包含 SPS/PPS，可独立解码。
5. 暂时无帧继续 5 ms 重试并处理 RTSP/保活。无人观看时暂停消费；重新观看时清除预读旧帧，从队列最新可用 IDR 恢复。
6. TEARDOWN 只关闭对应订阅。默认客户端出站队列最大 4 MiB，持续 2 秒无发送进展或收尾超时会回收连接。
7. Ctrl+C/Ctrl+Break 只设置原子停止标记，主线程负责停止和回收。启动等待每隔最多 20 ms 检查该标记，随后通知输入并 join；最终释放时间取决于设备驱动响应停止的速度。
8. 正常结束/服务器停止沿用最终 RTCP SR/SDES/BYE 排空；输入失败由消费者收到异常并终止订阅，服务器仍监听，后续 PLAY 返回 455，自动重连留待阶段 6。

## 自动验证结果

MSVC Debug，2026-11-09：

| 配置 | CTest | 耗时 |
| --- | --- | --- |
| 默认 `build` | 16/16 通过 | 22.54 秒 |
| 仅编码 `build-encoder` | 22/22 通过 | 26.52 秒 |
| 采集加编码 `build-camera-encoder` | 25/25 通过 | 27.64 秒 |

包含原有文件模式、多轨道、事件循环、RTCP 和双进程集成测试。新增验证包括参数互斥/溢出/中文输入文件、首帧等待取消、绑定失败释放、Waiting/失败/停止、TEARDOWN 隔离、无人观看后最新 IDR 恢复、真实 TCP 分片重组和解码。

另外验证 MinGW Release 的服务器构建及 `--help` 成功；仅采集、不启用编码的服务器也构建成功，`--list-cameras` 正常枚举，推流明确提示需要同时开启采集和编码。

共享流测试：A 解码 15 帧，B 在 P 帧区间加入且不接收这些 P 帧，随后从 IDR 解码 9 帧，与 A 的第 6～14 帧逐帧哈希一致；A 的 RTP 时间为 0、3000、6000……，确认使用 30 fps 编码时间而非文件模式 25 fps。每个客户端使用独立 SSRC、序号和相对时间。

慢客户端测试：640x360、30 fps、高码率噪声图像经过真实编码；一个客户端缩小接收缓冲并停止读取，另一个持续接收和解码，验证慢连接回收及正常连接后续进展，实际覆盖 FU-A。

独立 FFmpeg 双客户端验收命令：

```powershell
python tests/verify_camera_rtsp.py `
    --fixture build-camera-encoder/Debug/camera_rtsp_tests.exe `
    --ffmpeg .verify/stage3-sdk/ffmpeg-master-latest-win64-gpl-shared/bin/ffmpeg.exe `
    --output .verify/stage5-network
```

脚本启动测试程序的 `--serve` 入口，12 秒后自动停止；生成帧沿同一实时源和服务器链路输出，生产服务器不提供生成帧选项。验收结果：A 90 帧，B 30 帧，B 对应 A 从零计数的第 18 帧起的连续序列，逐帧 MD5 匹配；两个 FFmpeg 的错误日志均为空。结果和日志保存在 `.verify/stage5-network/`。

## 实机待验收

- [ ] 用实际 USB 摄像头启动，VLC 和 ffplay 同时播放，确认画面和局域网首帧等待时间。
- [ ] 第二个播放器中途加入，关闭任意播放器后另一播放器持续观看。
- [ ] 全部退出后再次观看，确认恢复实时画面。
- [ ] 物理拔出摄像头，确认错误提示和连接结束，进程不崩溃。
- [ ] 持续采集时 Ctrl+C/Ctrl+Break 停止，再次启动可重新打开设备。

当前设备枚举为 0，上述项目没有标记通过。自动生成帧及 FFmpeg 网络验收不替代设备驱动、真实画面和物理拔出验证。
