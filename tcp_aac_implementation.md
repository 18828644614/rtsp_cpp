# TCP 多轨道与 AAC：实现说明

阶段 6 已将下述媒体与连接逻辑迁移到同一个 select 事件循环，详见 [事件循环实现说明](event_loop_implementation.md)。轨道、时间线和封包规则保持不变。

## 1. 框架

```text
RtspServer
└── 一个共享 MediaSession
    ├── H264MediaSource → H264Packetizer ─┐
    ├── AacFileReader  → AacPacketizer  ─┤
    ├── 一个调度循环，按媒体时间发送 ←───┘
    └── 多个 MediaSubscription
        ├── 客户端 A
        │   ├── 视频 RtpSender ─┐
        │   ├── 音频 RtpSender ─┤
        │   └── OutboundQueue ←─┘ → on_writable → TCP
        └── 客户端 B：独立 Sender、队列和发送进度
```

每个文件启动时只加载一次，每帧负载只封装一次。每个客户端只为自己 SETUP 的轨道建立 Sender。

使用普通结构体、函数、循环和现有的智能指针。阶段 6 新增一个小型 EventLoop；媒体与连接均不再创建后台线程，不引入继承、回调或模板框架。

## 2. 五处修改

| 模块 | 修改内容 |
| --- | --- |
| `SessionDescription` | 用 `tracks` 保存轨道集合；每条轨道有媒体类型、编码、时钟、声道、fmtp 和 control |
| `RtspServerSession` | 用 `TrackState` 集合保存每轨道的 SETUP 状态和 TCP 通道 |
| `RtpSender` | 接收通用 `RtpPayloadUnit`；负责序号、SSRC、RTP 头和 TCP 头 |
| `MediaSubscription` | 为已 SETUP 的轨道建立 Sender，保存该客户端的共同媒体起点 |
| `MediaSession` | 持有视频、音频源，一个循环比较两条轨道的下一帧时间 |

AAC 新增三个小模块：`AacAccessUnit` 保存一帧，`AacFileReader` 读取 ADTS，`AacPacketizer` 生成 RTP 负载。

## 3. 控制流程

```text
DESCRIBE /live
    → SDP：H264 trackID=0、AAC trackID=1

SETUP /live/trackID=0
    → 保存视频通道，返回 Session ID

SETUP /live/trackID=1，携带同一个 Session ID
    → 保存音频通道，检查与已协商通道是否冲突

PLAY /live
    → 先入队 PLAY 成功响应，再开始投递已 SETUP 轨道

TEARDOWN /live
    → 取消全部轨道投递，再入队 TEARDOWN 响应
```

可以只 SETUP 视频或音频，也可以先 SETUP 音频。同一轨道重复 SETUP 返回 455；第二条轨道缺失 Session 返回 454；通道冲突或 UDP Transport 返回 461。播放中不支持追加轨道。

TCP 通道由客户端协商，例如视频 RTP/RTCP 为 0/1、音频为 2/3。一个连接的 RTSP、RTP 和未来 RTCP 继续共用一个有序队列。

## 4. AAC 数据流程

```text
ADTS AAC 文件
    → 检查同步字、profile、采样率、声道和帧长度
    → 去除 7 字节 ADTS 头，或带 CRC 的 9 字节头
    → AacAccessUnit：裸 AAC、PTS、1024 个采样
    → AacPacketizer：AU Header + 裸 AAC
    → RtpSender：RTP 头 + TCP interleaved 头
```

第一版固定 AAC-LC、采样率和声道配置，每个 ADTS 帧只包含一个 raw data block。根据 ADTS 配置生成 AudioSpecificConfig 和 MPEG4-GENERIC / AAC-hbr SDP。

每个 RTP 包携带一个完整 AAC AU，不实现多个 AU 聚合或 AU 分片。AU-size 为 13 位字节数，AU-index 为 0；AU-headers-length 为 16 位。带 CRC 的 ADTS 只跳过 CRC 字段，不执行校验。

文件上限为 64 MiB，不接受空文件、截断帧、PCE 声道配置、配置中途变化或不支持的 profile。输入不是 HE-AAC。

## 5. 时间线

```text
视频：PTS = 帧号 × 3600，时钟 = 90000 Hz
音频：PTS = AU 序号 × 1024，时钟 = 音频采样率

媒体时间 = PTS / 时钟频率
发送时刻 = 共同的墙钟起点 + 媒体时间
```

调度器各预读一帧，比较媒体时间，先发送较早的一帧；时间相同时先视频。每次由累计 PTS 计算绝对发送时刻，不逐帧累加截断的毫秒值。

客户端加入后：

- 订阅视频：等待下一个 IDR，以 IDR 的媒体时间作为该客户端所有轨道的共同起点。
- 只订阅音频：从当前音频 AU 开始，无需等待视频 IDR。
- 各轨道将共同起点换算为自己的 RTP tick；保留首个音频 AU 相对于 IDR 的时间差。
- 没有订阅者时暂停共享进度，恢复时重新建立墙钟映射，不补发暂停期间的数据。
- 一条轨道结束后继续发送另一条，所有源及预读帧结束后才关闭订阅队列。
- 文件结束后不自动重播，后续 PLAY 返回 455。

这些规则用于服务器发送调度。视频仍固定 25 fps；不读取真实 PTS/DTS。用于音画同步的测试视频应无 B 帧，音视频输入应具有一致的起点。

裸 H.264 和 ADTS 文件不保存原始容器中的音视频起始偏移，本实现假设两者均从时间零点开始。完整 RTCP Sender Report、真实容器时间戳和长期播放器同步验收仍需后续补齐。

## 6. 运行

构建与回归：

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

双轨道：

```powershell
.\build\Debug\rtsp_server.exe 8554 127.0.0.1 samples/sample.h264 samples/sample.aac
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

纯音频，使用 `-` 关闭视频：

```powershell
.\build\Debug\rtsp_server.exe 8554 127.0.0.1 - samples/sample.aac
```

原来的单视频命令保持可用。自带 `rtsp_client` 能解析双轨道 SDP，并选择 H.264 保存文件；音频收听由 ffplay 等外部播放器完成。它暂不支持保存 AAC，连接纯音频资源时会给出明确错误。

## 7. 阅读顺序与验证

1. `src/sdp/session_description.h`：轨道长什么样。
2. `src/rtsp/rtsp_server_session.cpp`：如何分别 SETUP。
3. `src/media/aac_file_reader.cpp`、`aac_packetizer.cpp`：AAC 怎样读取和封包。
4. `src/rtp/rtp_payload.h`、`rtp_sender.cpp`：公共负载怎样成为 TCP RTP 包。
5. `src/media/media_session.cpp`：先看 `MediaSubscription::deliver()`，再看 `MediaSession::tick()`。
6. `src/rtsp/rtsp_server_connection.cpp`：PLAY 时怎样为已 SETUP 的轨道建立订阅。

测试覆盖 AAC 固定字节、ADTS 边界、双轨道 SDP、不同 SETUP 顺序、通道冲突、独立 RTP 状态、统一时间线、晚加入、取消、纯音频和不同轨道 EOF。已有单视频测试继续保留。

本次 Debug 构建与 11 组 CTest 全部通过。另用 FFmpeg 8.1 经真实 TCP 连接接收 3 秒样例：75 帧 H.264 解码后的逐帧 MD5 与源文件一致，AAC 解码得到的 581632 字节 PCM 与源文件完全一致；纯音频接收和自带客户端从双轨道资源保存视频也通过。外部校验结果保存在本机 `build/av_verification/result.json`，不将生成媒体加入 Git。
