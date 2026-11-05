# USB 摄像头实时采集与 RTSP 输出实现框架

本文档描述如何在当前 `rtsp-cpp` 项目基础上，增加“读取电脑 USB 摄像头画面，并通过 RTSP 提供给 VLC、ffplay 或其他 RTSP 客户端播放”的能力。

本文档是实施框架，不代表所有功能已经完成。实施过程中应保持现有 H.264 文件播放、RTSP 会话、RTP 封包和测试继续可用。

## 一、目标

第一版目标限定为：

- Windows 电脑上的一个 USB 摄像头；
- 视频分辨率和帧率可配置，默认 1280x720、25 fps；
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

当前 `H264MediaSource` 内部直接持有 `H264FileReader`。建议先提取统一的实时视频源接口，然后保留文件源作为回归实现：

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

建议实现：

```text
FileVideoSource
    └─ 复用当前 H264FileReader，保持现有文件模式

CameraVideoSource
    ├─ 打开摄像头设备
    ├─ 采集原始帧或接收摄像头编码帧
    ├─ 编码为 H.264
    ├─ 提取 SPS/PPS
    ├─ 组织访问单元和时间戳
    └─ 通过有界队列提供给 MediaSession
```

第一版推荐使用 FFmpeg 的设备输入和 H.264 编码，原因是它可以减少 Windows 摄像头格式、像素转换和编码器适配工作。后续如果需要减少外部依赖，再把采集实现替换为 Media Foundation，`CameraVideoSource` 对上层保持不变。

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

- [ ] 定义 `VideoSource` 接口；
- [ ] 将当前文件源改成 `FileVideoSource`；
- [ ] 保持 `MediaSession` 的文件播放结果不变；
- [ ] 回归现有 CTest 和 FFmpeg 文件传输验证。

### 阶段 2：建立摄像头采集最小链路

- [ ] 枚举并打开 Windows 摄像头；
- [ ] 固定一个分辨率和帧率；
- [ ] 采集一帧并记录像素格式、时间戳；
- [ ] 验证摄像头断开和停止释放。

### 阶段 3：加入 H.264 编码

- [ ] 将摄像头帧编码为 H.264；
- [ ] 读取 SPS/PPS；
- [ ] 输出完整访问单元、PTS、duration 和 IDR 标志；
- [ ] 用独立文件或 FFmpeg 解码器验证编码结果。

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
