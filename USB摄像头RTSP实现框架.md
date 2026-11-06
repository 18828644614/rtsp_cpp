# USB 摄像头实时采集与 RTSP 输出实现框架

本文档描述如何在当前 `rtsp-cpp` 项目基础上，增加“读取电脑 USB 摄像头画面，并通过 RTSP 提供给 VLC、ffplay 或其他 RTSP 客户端播放”的能力。

本文档是实施框架，不代表所有功能已经完成。实施过程中应保持现有 H.264 文件播放、RTSP 会话、RTP 封包和测试继续可用。

## 一、目标

第一版目标限定为：

- Windows 电脑上的一个 USB 摄像头；
- 视频分辨率和帧率可配置，当前采集与编码默认 1280x720、30 fps；
- 摄像头画面编码为 H.264；
- 通过现有 RTSP 服务端输出；
- 使用 RTP over RTSP/TCP，优先保证局域网内稳定播放；
- 支持多个客户端同时观看同一个实时视频；
- 支持 VLC、ffplay 等外部播放器；
- 摄像头断开、采集失败或客户端退出时能够释放资源。

第一版暂不要求：音频、录像、认证、UDP、云台控制、复杂设备管理、H.265 和多摄像头。

## 二、当前项目可以复用的部分

当前项目已经完成了实时输出所需的大部分协议层：

| 现有模块 | 摄像头模式中的作用 |
| --- | --- |
| `RtspServer` | 监听端口、管理连接和驱动事件循环 |
| `RtspServerConnection` | 处理每个客户端的 RTSP 请求和 TCP 发送 |
| `RtspServerSession` | 处理 OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN 状态 |
| `SessionDescription` | 生成 H.264 SDP、描述 payload type 和控制地址 |
| `H264Packetizer` | 将完整 H.264 NALU 封装成 RTP 单包或 FU-A |
| `RtpSender` | 为每个客户端维护序号、时间戳和 SSRC |
| `OutboundQueue` | 限制每个客户端的发送缓存，隔离慢客户端 |
| `EventLoop` | 处理 RTSP TCP 读写和媒体发送期限 |
| `MediaSubscription` | 向客户端分发访问单元，等待视频 IDR 后开始发送 |

当前需要替换或扩展的是 `H264MediaSource` 和 `MediaSession` 对文件输入的假设。

2026-11-05 阶段 1 已完成：原 `H264MediaSource` 已迁为 `FileVideoSource` 并实现 `VideoSource`，`MediaSession` 持有接口。摄像头采集、编码、队列和实时等待调度仍按后续阶段实施。

## 三、目标数据流

```text
USB 摄像头
    │
    │ DirectShow / Media Foundation / FFmpeg 输入
    ▼
原始帧或摄像头自带的 MJPEG/H.264
    │
    │ 像素格式转换、H.264 编码、关键帧控制
    ▼
CameraVideoSource
    │  H264AccessUnit + PTS + duration + is_keyframe
    ▼
MediaSession
    │  实时调度、SPS/PPS 缓存、订阅者分发
    ▼
H264Packetizer → RtpSender → OutboundQueue
    ▼
RTSP over TCP
    ▼
VLC / ffplay / 其他 RTSP 客户端
```

采集和编码不应直接运行在 RTSP 事件循环中。推荐使用一个采集线程和一个有界帧队列：

```text
采集/编码线程 ──► CameraFrameQueue ──► RTSP 事件循环
                         │
                         └─ 队列满时丢弃旧帧，等待新的 IDR
```

网络事件循环仍然只负责 Socket、会话和已经准备好的编码帧，不应因为摄像头读取阻塞而停止处理其他客户端。

## 四、媒体源抽象

改造前 `H264MediaSource` 内部直接持有 `H264FileReader`。阶段 1 已提取 `src/media/video_source.h` 中的视频源接口，并保留文件源作为回归实现：

```cpp
class VideoSource {
public:
    virtual ~VideoSource() = default;

    // 返回 true 表示取到一帧；false 表示当前没有新帧或源已经结束。
    virtual bool read(H264AccessUnit& frame) = 0;

    // 实时摄像头没有文件 EOF，需要单独区分“暂时无帧”和“永久结束”。
    virtual bool ended() const = 0;

    virtual const MediaTrack& track() const = 0;
};
```

当前文件源实现与后续摄像头源规划：

```text
FileVideoSource
    └─ 已实现，复用 H264FileReader，保持现有文件模式

CameraVideoSource
    ├─ 后续阶段：打开摄像头设备
    ├─ 采集原始帧或接收摄像头编码帧
    ├─ 编码为 H.264
    ├─ 提取 SPS/PPS
    ├─ 组织访问单元和时间戳
    └─ 通过有界队列提供给 MediaSession
```

第一版推荐使用 FFmpeg 的设备输入和 H.264 编码，原因是它可以减少 Windows 摄像头格式、像素转换和编码器适配工作。后续如果需要减少外部依赖，再把采集实现替换为 Media Foundation，`CameraVideoSource` 对上层保持不变。

文件源位于 `src/media/file_video_source.h/.cpp`，只暴露一条 `MediaTrack`。`MediaSession` 使用 `unique_ptr<VideoSource>` 接管源，并组装完整 SDP；路径构造函数自动创建 `FileVideoSource`，原服务端入口不需修改。文件读取、SPS/PPS 时序缓存、IDR 参数补齐、固定 3600 tick 的视频时间以及 EOF 后拒绝重播均保留。接口已预留“无帧但未结束”的语义，`MediaSession` 对该状态的短期唤醒处理留在阶段 4。

## 五、摄像头采集与编码模块

建议新增以下文件：

```text
src/media/camera_video_source.h/.cpp   // 对外提供 H264AccessUnit
src/media/camera_capture.h/.cpp       // 摄像头设备采集
src/media/h264_encoder.h/.cpp          // 原始帧到 H.264 编码
src/media/video_frame_queue.h/.cpp     // 采集线程到事件循环的有界队列
```

如果第一版直接调用 FFmpeg 命令行，也可以先把采集和编码封装在 `camera_video_source` 中，等协议链路验证成功后再拆分。长期方案应优先使用 FFmpeg 库或 Media Foundation API，避免依赖 shell 管道的进程生命周期和错误处理。

摄像头模块至少需要提供这些配置：

```text
device                 摄像头名称或设备编号
width / height         输出分辨率
fps                    输出帧率
bitrate                H.264 目标码率
gop                    关键帧间隔，建议 1～2 秒
pixel_format           采集像素格式或编码器输入格式
```

H.264 编码第一版建议关闭 B 帧，使用低延迟设置，并固定产生 SPS/PPS。新客户端加入时必须能在下一个 IDR 处开始解码。

## 六、`MediaSession` 的实时化修改

当前文件模式假设：读取失败代表文件结束，之后发送最终 RTCP 并关闭订阅。摄像头模式必须改成三种状态：

```text
Running       摄像头正常工作，可以取到新帧
Waiting       暂时没有新帧，保持连接，不发送 EOF
Ended/Failed  摄像头永久结束、断开或发生不可恢复错误
```

调度逻辑应遵循：

1. 从 `CameraVideoSource` 读取已经编码好的下一帧；
2. 如果暂时没有帧，设置短期唤醒时间并返回事件循环；
3. 如果有帧，按照 PTS 计算发送时间；
4. 向当前订阅者分发访问单元；
5. 如果队列积压，丢弃过时的非关键帧；
6. 丢帧后等待下一个 IDR，再恢复向客户端发送；
7. 只有摄像头明确结束或失败时，才调用现有的 `finish()` 流程。

实时流不能沿用“没有订阅者就一直读取文件直到 EOF”的假设。没有观看者时可以暂停编码，也可以持续采集但只保留最新关键帧，具体策略在第一版统一为：暂停向 `MediaSession` 推进，重新有订阅者后从最新可用 IDR 开始。

## 七、线程与生命周期

推荐的生命周期如下：

```text
RtspServer::run()
    ├─ 创建 CameraVideoSource
    ├─ 启动采集/编码线程
    ├─ 事件循环读取 CameraFrameQueue
    ├─ 连接结束或服务器停止
    ├─ 停止接收新帧
    ├─ 请求采集线程退出
    ├─ 等待线程结束
    └─ 释放摄像头、编码器和队列
```

约束：

- 摄像头线程不能直接操作 `TcpSocket`、`RtspServerConnection` 或客户端队列；
- 采集线程与事件循环之间只通过线程安全的有界队列传递已编码帧；
- 队列必须有最大帧数和最大字节数；
- 停止流程必须先停止生产，再唤醒并等待消费者退出；
- 摄像头拔出后不能继续使用失效的设备句柄；
- 队列满时不能无限等待，否则会造成摄像头线程和服务器停止。

## 八、命令行与配置

建议保留当前文件模式，并增加摄像头模式，不要改变已有命令的含义：

```powershell
# 文件模式，保持现有行为
rtsp_server.exe 8554 127.0.0.1 samples/sample.h264

# 摄像头模式示例
rtsp_server.exe --camera 0 --port 8554 --bind 0.0.0.0 `
    --width 1280 --height 720 --fps 25 --bitrate 2000000
```

实际参数解析可以先采用位置参数，稳定后再改为命名参数。SDP 中的 H.264 `profile-level-id` 和 `sprop-parameter-sets` 必须根据摄像头编码器实际输出生成，不能继续假定文件源的 SPS/PPS。

## 九、分阶段实施顺序

### 阶段 1：抽象媒体源

- [x] 定义 `VideoSource` 接口；
- [x] 将当前文件源改成 `FileVideoSource`；
- [x] 保持 `MediaSession` 的文件播放结果不变；
- [x] 回归现有 CTest 和 FFmpeg 文件传输验证。

阶段 1 验证记录（2026-11-05）：

- MSVC Debug 构建成功；`ctest --test-dir build -C Debug --output-on-failure` 全部 13 组通过，耗时 21.29 秒。
- `media_file_tests` 增加通过 `VideoSource` 读取文件的接口契约验证，覆盖 SDP、按播放顺序更新 SPS/PPS、IDR 参数补齐、时间信息和 EOF 清空输出。
- `media_session_tests` 增加文件路径构造与源注入构造的对照，SDP、调度期限、EOF 状态及 RTSP/RTP/RTCP 入队字节一致。
- 复用本机 `build/verify_shared_media.py`：含 B 帧的 100 帧 H.264 文件，首个客户端收到全部 100 帧，晚加入客户端收到后续 75 帧，FFmpeg 解码逐帧 MD5 分别匹配完整输入和对应后缀。输入只保留开头的 SPS/PPS，后续 IDR 参数由文件源补齐。
- 复用本机 `.verify/verify_multitrack.py`：FFmpeg TCP 接收 75 帧视频与 AAC，纯音频、自带客户端视频均解码匹配；AAC-LC 为 48 kHz 双声道，解码 PCM 共 581632 字节，SHA-256 为 `cf98411f665ce05d2ac45210186b9cf3915bbf02c329a0ea217da091a7d4cf53`。

上述 FFmpeg 脚本和生成的媒体/日志位于本机忽略目录，FFmpeg 仍只用于验收，不参与 C++ 编译或运行时文件播放。

### 阶段 2：建立摄像头采集最小链路

- [x] 枚举并打开 Windows 摄像头；
- [x] 固定一个分辨率和帧率；
- [x] 采集一帧并记录像素格式、时间戳；
- [ ] 验证摄像头断开和停止释放。

阶段 2 实现与验证记录：

- 新增独立 `CameraCapture`、持有原始帧引用的 `CameraFrame` 和 `camera_probe`；默认不启用摄像头构建，不修改 `VideoSource` 或 RTSP 文件链路。
- 使用 DirectShow 枚举视频设备，FFmpeg 库打开并读取 rawvideo；首版明确支持 `yuyv422` 和 `bgr24`，不接入 MJPEG 或 H.264 编码。
- 本机 `Integrated Camera` 已验证 1280x720、30 fps、`yuyv422`，实际 stride 为 2560、时间基为 1/10000000。该设备未列出 25 fps，阶段 2 使用 30 fps。
- 单帧采集后关闭并连续重新打开三次通过；10 秒连续采集取得 302 帧，接收统计约 30.12 fps。
- 原有 13 组 CTest 加 3 组摄像头无硬件检查全部通过；实机另验证帧引用跨读取/关闭有效、时间戳推进、跨线程停止和重新打开。
- 持续采集时发送 Ctrl+Break，约 0.45 秒完成停止和释放，退出码 0，随后重新打开成功；关闭摄像头开关也可独立构建原客户端和服务器。
- 停止释放已验证；设备丢失错误与无帧超时路径已实现。物理拔出仍需外接 USB 摄像头人工验收，因此最后一项保持未勾选。

详细构建、命令和验收步骤见 [摄像头采集阶段 2 实现说明](camera_capture_implementation.md)。

### 阶段 3：加入 H.264 编码

- [x] 将摄像头帧编码为 H.264；
- [x] 读取 SPS/PPS；
- [x] 输出完整访问单元、PTS、duration 和 IDR 标志；
- [x] 用独立文件或 FFmpeg 解码器验证编码结果。

阶段 3 实现与验证记录（2026-11-06）：

- 独立 `H264Encoder` 消费阶段 2 的 `CameraFrame`，内部通过 libswscale 转 YUV420P，再由 libx264 低延迟编码，无 B 帧，输出带 AUD 的完整 AU；NALU 不含起始码，IDR 携带 SPS/PPS。
- 复用 `H264AccessUnit` 的 90 kHz PTS/duration 与 IDR 标志；保留有效源时间关系，明确区分缺失时间戳时的帧率推算，并检查不递增、时间基改变和溢出。
- 新增 `ENABLE_H264_ENCODING`，可只开启编码；`h264_encode_probe` 支持生成帧与可选的真实设备输入，输出 Annex-B 文件和逐 AU CSV 时间记录。
- 无摄像头专项测试用 YUYV422/BGR24、多帧率与多 slice 编码后实际解码，核对彩色图像、时间、参数集、IDR、排空和文件读取兼容性；默认构建 13/13、仅开启编码 17/17、采集加编码 20/20 CTest 通过，MinGW Release 编码探测构建和运行通过。
- 1280x720、30 fps 生成帧连续编码 300 帧，独立 FFmpeg 解码无错误，ffprobe 确认无 B 帧、YUV420P、300 帧。当前机器无摄像头，真实设备采集接编码的实机验收待补；阶段 4/5 的线程、队列和实时 RTSP 接入尚未实施。

接口、构建、时间规则和完整验证命令见 [H.264 编码阶段 3 实现说明](h264_encoder_implementation.md)。

### 阶段 4：接入 `CameraVideoSource`

- [ ] 增加采集/编码线程；
- [ ] 增加线程安全有界队列；
- [ ] 实现“无新帧”和“已结束”的区分；
- [ ] 将实时帧交给 `MediaSession`。

### 阶段 5：接入 RTSP 输出

- [ ] 由摄像头 SPS/PPS 生成 SDP；
- [ ] 复用现有 H.264 RTP 封包；
- [ ] 验证首个客户端等待 IDR 后正常播放；
- [ ] 验证第二个客户端可以中途加入；
- [ ] 验证慢客户端不会阻塞其他客户端。

### 阶段 6：完善运行时处理

- [ ] 增加摄像头断开提示和可选重连；
- [ ] 增加队列溢出统计；
- [ ] 增加采集延迟、编码延迟和发送延迟日志；
- [ ] 增加启动参数和帮助文档；
- [ ] 增加摄像头模式集成测试和手工验收记录。

## 十、验收标准

最低验收标准：

1. 摄像头模式启动后，VLC 可以打开 `rtsp://电脑IP:8554/live`；
2. `ffplay -rtsp_transport tcp rtsp://电脑IP:8554/live` 可以看到实时画面；
3. 首帧延迟在局域网环境下可接受，播放不会等待文件 EOF；
4. 两个客户端可以同时观看；
5. 第二个客户端加入后能在下一个 IDR 播放；
6. 关闭一个客户端不会影响另一个客户端；
7. 摄像头拔出后服务器不会崩溃，客户端最终收到连接结束或错误；
8. 停止服务器后采集线程、编码器和摄像头句柄都能释放；
9. 原有文件模式和现有 CTest 不回归。

## 十一、主要风险与处理方式

| 风险 | 处理方式 |
| --- | --- |
| 摄像头输出格式不统一 | 启动时协商格式；第一版固定支持一种输入格式 |
| 摄像头只输出 MJPEG 或 YUY2 | 增加像素格式转换和 H.264 编码 |
| 没有 SPS/PPS 或客户端中途加入黑屏 | 缓存 SPS/PPS，并在 IDR 前补发参数集 |
| 采集线程阻塞事件循环 | 使用独立采集线程和有界队列 |
| 队列积压导致延迟越来越大 | 丢弃旧非关键帧，保留最新可恢复的 IDR |
| H.264 分片过大 | 继续复用现有 FU-A 封包和 RTP MTU 限制 |
| 摄像头拔出 | 将设备错误转成源状态，执行清理或重连 |
| FFmpeg 或系统库部署困难 | 将采集/编码依赖集中在媒体模块，保留文件模式作为无依赖回退 |

## 十二、推荐的第一步

下一步不要立即修改 RTSP 会话层。先完成以下最小闭环：

```text
打开 USB 摄像头
    → 采集一帧
    → 编码一帧 H.264
    → 提取 SPS/PPS 和 IDR
    → 保存为临时 Annex-B 文件
    → 用 ffplay 验证文件可解码
```

这一步成功后，再把临时文件输出替换成 `CameraVideoSource` 的实时队列。这样可以把“摄像头采集/编码问题”和“RTSP 网络传输问题”分开定位，降低改造风险。
