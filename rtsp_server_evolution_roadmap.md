# RTSP 服务器演进路线

本文档描述 `rtsp-cpp` 从单客户端 H.264 教学版，逐步演进为支持多客户端、共享媒体源、TCP/UDP 传输、音视频多轨道和基本运行时管理的 RTSP 服务器的实施路线。

本文档只定义架构边界、实施顺序和验收标准，不要求一次性实现所有目标。每个阶段都应保持前一阶段的回归测试可运行。

**当前顺序调整（2026-10-26）：** 阶段 1～3 已完成；阶段 4 UDP 延后，优先完成阶段 5 的 TCP 多轨道与 AAC 基础版。UDP 不是多轨道的前置条件。当前实现框架、运行方式和范围见 [TCP 多轨道与 AAC 实现说明](tcp_aac_implementation.md)。

## 一、目标和范围

最终目标包括：

- 多个客户端可以同时连接同一个 RTSP 服务器。
- 多个客户端可以订阅同一个媒体资源，并共享同一个媒体源。
- 同时支持 RTP over RTSP/TCP 和 RTP over UDP。
- 一个 RTSP 资源可以包含 H.264 视频和 AAC 音频轨道。
- 每个客户端拥有独立的 RTSP 状态、Session ID、Transport、RTP 序列号、时间戳和 SSRC。
- 媒体源只读取一次，由服务器统一调度并向订阅者分发。
- 单个慢客户端不会阻塞其他客户端。
- 连接断开、文件结束、Session 超时和服务器停止时，相关资源能够正确释放。
- 后续支持基本 RTCP、保活、超时清理和更完整的异常处理。

第一阶段不追求完整 RTSP 服务器能力，暂不包含认证、鉴权、随机访问、录像管理、组播和复杂资源路由。

## 二、初始基线（阶段 0 之前）

当前项目已经具备：

- `RtspMessage`：RTSP 请求和响应的解析、生成。
- `RtspStreamParser`：TCP 字节流中的 RTSP 消息和 interleaved 帧拆分。
- `RtspServerSession`：`OPTIONS`、`DESCRIBE`、`SETUP`、`PLAY`、`TEARDOWN` 状态机。
- `SessionDescription`：单路 H.264 SDP 的生成和解析。
- `RtpPacket`：RTP 公共头的解析和生成。
- `H264Packetizer`：单 NALU 和 FU-A 封包。
- `H264Depacketizer`：单 NALU、FU-A 和 STAP-A 解包。
- `H264FileReader`：按 AUD 将 Annex-B 文件组织成访问单元，并提取 SPS/PPS。
- `H264FileWriter`：将接收到的 NALU 保存为 Annex-B 文件。
- CMake、CTest 和端到端媒体传输测试。

当前主要限制：

- `server_main.cpp` 只接受并处理一个客户端连接。
- `H264FileReader` 由服务器主循环直接持有，读取游标属于单个连接。
- `TcpTransport` 只描述 TCP interleaved 通道。
- `SessionDescription` 只包含一条视频轨道。
- `TcpSocket::send_all()` 是阻塞发送。
- 没有共享 `MediaSession`、媒体源抽象、连接管理器、发送队列和 UDP Socket 抽象。
- `H264AccessUnit` 尚未携带关键帧标记、PTS 和持续时间。

当前工作树的基线验证命令为：

```text
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

基线验收要求：

- 所有现有单元测试和端到端测试通过。
- 服务器和客户端可以完成一次完整的 H.264 文件传输。
- 测试使用本次构建产生的二进制，而不是旧的构建输出。

## 三、目标架构

```text
RtspServer
├── MediaSessionRegistry
│   └── MediaSession("/live")
│       ├── H264MediaSource
│       ├── AacMediaSource
│       ├── MediaClock / Scheduler
│       └── Subscribers
│           ├── Subscription A
│           └── Subscription B
├── RtspServerConnection A
│   ├── TcpSocket
│   ├── RtspStreamParser
│   ├── RtspServerSession
│   ├── TrackState
│   │   ├── Video Transport / RtpSender
│   │   └── Audio Transport / RtpSender
│   └── OutboundQueue
└── RtspServerConnection B
    ├── TcpSocket
    ├── RtspStreamParser
    ├── RtspServerSession
    ├── TrackState
    └── OutboundQueue
```

| 组件 | 职责 |
| --- | --- |
| `RtspServer` | 监听端口、接受连接、维护连接生命周期 |
| `MediaSessionRegistry` | 根据 RTSP 资源路径查找 `MediaSession` |
| `RtspServerConnection` | 管理一个客户端的 Socket、解析器、Session 和输出队列 |
| `RtspServerSession` | 处理 RTSP 状态、请求校验和响应生成，不直接读写 Socket |
| `TrackState` | 保存某条轨道的 SETUP 状态、Transport 和 `RtpSender` |
| `MediaSession` | 表示一个可播放的共享资源，并维护订阅关系 |
| `MediaSource` | 读取或产生带时间戳的媒体访问单元 |
| `Scheduler` | 根据媒体时间戳驱动媒体源和分发节奏 |
| `RtpSender` | 为一个客户端、一个轨道生成 RTP 头并投递到输出队列 |
| `OutboundQueue` | 串行发送 RTSP 响应、RTP 和 RTCP，限制队列长度 |
| `SessionDescription` | 描述资源级控制信息和一条或多条媒体轨道 |

### 3.1 状态和所有权原则

- RTSP Session 是客户端独有状态。
- Track Transport 是客户端和轨道独有状态。
- RTP 序列号、RTP 时间戳和 SSRC 是客户端和轨道独有状态。
- `MediaSource` 和 `MediaSession` 是资源级共享状态。
- 共享对象不能保存裸的客户端引用；订阅关系应使用明确的生命周期令牌、弱引用或连接管理器中的稳定句柄。
- 一个客户端连接只能有一个有序的输出通道。RTSP 响应、RTP 和 RTCP 不能由多个线程直接并发写入同一个 TCP Socket。

## 四、媒体分发的基本模型

共享媒体源时，应区分媒体访问单元、RTP 负载和客户端 RTP 头：

```text
MediaSource
    → H264AccessUnit / AacAccessUnit
    → 只读取一次
    → 只封装一次媒体负载
    → 每个客户端分别生成 RTP 头
    → 每个客户端分别进入自己的 OutboundQueue
```

“封包一次”只表示 H.264 或 AAC 负载封装可以复用。最终 RTP 包不能直接在所有客户端之间共享，因为每个客户端必须使用独立的：

- sequence number；
- timestamp；
- SSRC；
- TCP interleaved channel 或 UDP 目标端口；
- 发送策略和队列状态。

新客户端加入共享媒体源时，不能直接从当前任意访问单元开始发送。对于 H.264，至少需要：

1. 缓存当前有效的 SPS/PPS；
2. 等待下一个 IDR 访问单元；
3. 从 SPS/PPS 和 IDR 开始向新客户端发送；
4. 如果媒体源直到 EOF 都没有新的 IDR，应按明确的策略拒绝加入、等待、回放或关闭连接。

因此，`H264AccessUnit` 后续需要携带至少以下信息：

```cpp
struct H264AccessUnit {
    std::vector<std::string> nalus;
    bool is_keyframe = false;
    std::uint64_t pts = 0;
    std::uint32_t duration = 0;
};
```

## 五、分阶段实施

### 阶段 0：恢复稳定基线

**难度：低。**

**目标：** 确保项目能够从源码重新配置、编译和运行测试。

**工作内容：**

- 修复 `src/rtp/rtp_packet.h` 中影响编译的 RTP 示意图或注释问题。
- 重新执行 CMake 配置、Debug 构建和 CTest。
- 确认测试使用本次构建产生的二进制。
- 保留当前单客户端 H.264 闭环作为回归基线。

**验收标准：**

```text
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

所有现有测试通过，服务器和客户端完成一次完整的 H.264 文件传输。

### 阶段 1：多客户端连接和连接生命周期

**难度：中等。**

**目标：** 在不引入共享媒体源的情况下，先验证多个独立 RTSP 会话可以同时工作。

**工作内容：**

- 新增 `RtspServerConnection`，避免与已有的 `RtspClient` 混淆。
- 新增 `RtspServer`，负责监听、接受连接和维护连接表。
- 将当前 `server_main.cpp` 中的单客户端循环移动到连接对象。
- 每个连接独立拥有 `TcpSocket`、`RtspStreamParser`、`RtspServerSession`、RTP 状态和 TCP interleaved 通道。
- 每个连接独立创建 `H264FileReader`，暂时允许各自从文件开头播放。
- 使用可管理的工作线程模型；线程必须能够被连接管理器回收，不能让连接线程持有已释放对象。
- 使用每次连接唯一的 Session ID，不能继续使用当前基于秒级时间的 ID 生成方式。

**此阶段的数据流：**

```text
客户端 A → 独立 H264FileReader → 客户端 A
客户端 B → 独立 H264FileReader → 客户端 B
```

**验收标准：**

- 两个客户端可以同时完成 `OPTIONS`、`DESCRIBE`、`SETUP`、`PLAY`。
- 两个客户端都能收到完整、可解码的 H.264 文件。
- 一个客户端执行 `TEARDOWN` 不会导致另一个客户端断开。
- 一个客户端断开后，服务器仍可接受新的客户端。
- 多客户端测试可以重复运行，不依赖固定 Session ID 或固定连接顺序。

### 阶段 2：统一 Transport、RtpSender 和出站队列

**难度：中等偏高。**

**当前状态：已完成 TCP interleaved 版本。** 每个连接拥有有界出站队列和单一发送线程，`RtpSender` 负责独立 RTP 状态；队列溢出时终止对应的慢客户端。共享媒体源、关键帧跳转和 UDP 仍按后续阶段实施。

**目标：** 在共享媒体源之前，先解决每个客户端的输出隔离和慢客户端问题。

**建议新增或重构：**

```text
src/rtp/rtp_sender.h
src/rtp/rtp_sender.cpp
src/net/outbound_queue.h
src/net/outbound_queue.cpp
src/rtsp/media_transport.h
src/rtsp/media_transport.cpp
```

**工作内容：**

- 将 RTSP 响应、RTP 数据和未来的 RTCP 数据统一放入客户端的出站队列。
- 由单一发送者按顺序向 Socket 写入数据。
- 队列设置最大长度或最大字节数。
- 队列满时明确处理：断开严重落后的客户端，或丢弃可重建的视频数据并等待关键帧。
- 让 `RtpSender` 只关心一条轨道和一个客户端的 RTP 状态。
- 将 TCP interleaved 作为 `MediaTransport` 的一种实现，为后续 UDP 保留接口。

**重要约束：**

- 不能让媒体线程直接对多个客户端调用阻塞式 `send_all()`。
- 不能让 RTSP 响应和 RTP 由两个线程绕过队列直接写入同一个 TCP Socket。
- 发送队列必须具备明确的关闭状态，连接断开后不能继续访问连接对象。

**验收标准：**

- 人为降低一个客户端的读取速度，其他客户端仍能继续接收。
- 一个客户端的队列达到上限后，不会阻塞媒体源和其他客户端。
- 客户端断开时，发送线程、队列和连接对象能够安全退出。
- RTSP 响应与 RTP interleaved 帧保持完整、有序。

### 阶段 3：共享 H.264 MediaSession

**难度：中等偏高。**

**当前状态：已完成单路 H.264、TCP interleaved 版本。** 服务器加载文件一次，`MediaSession` 统一调度并封装共享负载；`MediaSubscription` 为每个客户端保存独立 `RtpSender`，通过弱引用维护订阅，取消后不再投递。新订阅等待 IDR，缺失的 SPS/PPS 由媒体源补齐；最后一个订阅退出后暂停并保留游标，EOF 后关闭订阅，后续 PLAY 返回 455，不自动重播。媒体时间仍为 25 fps、90 kHz 的简化时间，不解析真实 PTS/DTS。

**目标：** 一个媒体源只读取一次，并向多个客户端分发。

**建议新增：**

```text
src/media/media_source.h
src/media/media_source.cpp
src/media/media_session.h
src/media/media_session.cpp
```

**工作内容：**

- `MediaSession` 持有一个 H.264 媒体源和一个媒体调度器。
- 第一个客户端进入 `PLAY` 后启动媒体源；没有订阅者时停止或释放调度任务。
- 媒体源每次读取一个带时间戳的 `H264AccessUnit`。
- 访问单元在客户端之间以不可变数据共享，例如 `std::shared_ptr<const H264AccessUnit>`。
- H.264 负载只封装一次；每个客户端独立生成 RTP 头并投递到自己的队列。
- 客户端在 `PLAY` 成功后订阅；`TEARDOWN`、Socket 断开或输出队列失败时取消订阅。
- 缓存 SPS/PPS，识别 IDR，并让新客户端从下一个可用关键帧开始播放。

**第一版播放策略：**

- 所有订阅者共享一个播放进度。
- 资源在第一个有效 `PLAY` 后开始播放。
- 文件结束时关闭该媒体资源的订阅，向后续客户端返回明确的结束结果。
- 暂不支持任意位置 seek 和每个客户端独立播放进度。
- 循环播放作为后续功能，不与第一版共享进度混在一起实现。

**验收标准：**

- 服务器只打开一次 H.264 文件。
- 多个客户端收到相同的访问单元序列。
- 一个客户端退出不影响其他客户端。
- 新加入客户端能够在 SPS/PPS 和下一个 IDR 后开始解码。
- 文件结束时所有订阅关系和连接资源都能正确清理。

### 阶段 4：RTP over UDP

**难度：中等偏高。**

**当前安排：延后。** 先完成阶段 5 的 TCP 版本，下面的 UDP 目标和验收保留到恢复此阶段时执行。

**目标：** 支持播放器通过 UDP 接收 RTP，同时保留 TCP interleaved。

**建议新增：**

```text
src/net/udp_socket.h
src/net/udp_socket.cpp
```

**Transport 应按客户端和轨道分别保存：**

```cpp
enum class TransportMode {
    InterleavedTcp,
    Udp
};

struct MediaTransport {
    TransportMode mode = TransportMode::InterleavedTcp;
    int rtp_channel = 0;
    int rtcp_channel = 1;
    std::uint16_t client_rtp_port = 0;
    std::uint16_t client_rtcp_port = 0;
    std::uint16_t server_rtp_port = 0;
    std::uint16_t server_rtcp_port = 0;
};
```

**工作内容：**

- 解析 `Transport: RTP/AVP;unicast;client_port=x-y`。
- 根据 TCP 对端地址确定 UDP 目标地址，并保存每个客户端、每条轨道的目标端点。
- 创建并绑定 RTP、RTCP UDP Socket。
- 服务端分配合法的 RTP/RTCP 端口，通常使用相邻的偶数/奇数端口。
- 在 `SETUP` 响应中返回 `server_port`。
- RTP 通过 `sendto()` 发送到客户端 RTP 端口。
- 保证 TCP 和 UDP 客户端可以同时订阅同一个 `MediaSession`。
- 暂不支持组播、RTSP over HTTP 和复杂 NAT 穿透。

**验收标准：**

- `ffplay -rtsp_transport tcp` 可以播放。
- `ffplay` 的 UDP 模式可以播放。
- TCP 和 UDP 客户端可以同时连接同一个媒体资源。
- 不同 UDP 客户端的目标地址和端口互不混淆。
- UDP 连接释放后，服务端端口和订阅关系能够回收。

### 阶段 5：多轨道 SDP 和 AAC

**难度：高。**

**当前状态：TCP 基础版已实现。** 多轨道 SDP、每轨道 SETUP/Transport/RtpSender、AAC-LC ADTS 读取与 AAC-hbr 封包、统一媒体时间调度、视频 IDR 后音视频共同起点、独立轨道 EOF 已接通。支持单视频、双轨道、纯音频和按需订阅。自带客户端选择 H.264 保存，音频由外部播放器接收。UDP 验收延后；真实 PTS/DTS、完整 RTCP SR 和长期播放器同步仍待后续补齐。

**目标：** 从单路 H.264 扩展到 H.264 + AAC，并保持轨道状态和时间戳独立。

**SessionDescription 方向：**

```cpp
struct MediaTrack {
    std::string media_type;
    std::string control;
    int payload_type;
    std::string encoding;
    int clock_rate;
    int channels;
    std::string fmtp;
};

struct SessionDescription {
    std::string control = "*";
    std::vector<MediaTrack> tracks;
};
```

**建议新增：**

```text
src/media/aac_file_reader.h
src/media/aac_file_reader.cpp
src/media/aac_packetizer.h
src/media/aac_packetizer.cpp
```

**工作内容：**

- 将当前单个 `video` 字段重构为轨道集合。
- 按 `trackID` 查找轨道，并允许视频、音频分别执行 `SETUP`。
- 每条轨道保存独立的 Transport、序列号、时间戳、SSRC 和 `RtpSender`。
- 读取 ADTS AAC，解析采样率、声道数、AAC profile 和每个 AU 的长度。
- 去除 ADTS 头，生成 MPEG-4 Generic RTP AU Header。
- 根据 AudioSpecificConfig 生成正确的 AAC `a=fmtp`。
- 为 H.264 和 AAC 分别建立媒体时钟和 PTS。
- `PLAY` 使用统一媒体时间线调度两条轨道，而不是让音频和视频各自按线程发送。

**时钟约定：**

```text
H.264：通常使用 90000 Hz
AAC：使用音频采样率，例如 44100 Hz 或 48000 Hz
AAC-LC：每个 AU 通常包含 1024 个采样
```

**验收标准：**

- `DESCRIBE` 返回视频和音频两条轨道。
- 客户端可以分别对两条轨道执行 `SETUP`。
- TCP 模式下视频和音频使用不同的 interleaved 通道。
- UDP 模式下视频和音频使用各自的 RTP/RTCP 端口（延后到阶段 4）。
- 播放器能够同时播放 H.264 和 AAC。
- 音频时间戳不复用视频时间戳，长时间播放不会持续漂移。

**当前验证：** Debug 构建和 11 组 CTest 通过；外部 FFmpeg 通过 TCP 接收 75 帧无 B 帧 H.264 和 48 kHz 双声道 AAC-LC，视频逐帧解码哈希与源文件一致，音频解码 PCM 与源文件一致；纯音频和自带客户端选择视频也通过。此结果证明短样例传输与解码正确，不代表已验收长期音画同步。

### 阶段 6：事件循环和非阻塞网络

**难度：高。**

**目标：** 将每客户端线程模型替换为一个可以管理多个连接的事件驱动模型。

**工作内容：**

- 为 Socket 增加非阻塞模式和 `WSAEWOULDBLOCK` 处理。
- 增加 `EventLoop`，管理监听 Socket、客户端 Socket 和定时任务。
- 监听 Socket 可读时执行 `accept`。
- 客户端 Socket 可读时执行接收和 RTSP 解析。
- 客户端 Socket 可写时刷新 `OutboundQueue`。
- 使用定时器驱动媒体源，不在 Socket 处理函数中阻塞等待。
- 处理连接异常、半关闭、写缓冲区满、客户端超时和队列溢出。

Windows 第一版可以使用 `select` 或 `WSAPoll`。当前项目以 Windows 为目标，不需要在这个阶段引入 Linux `epoll`。

**验收标准：**

- 一个线程可以管理多个客户端连接。
- 慢客户端不会阻塞其他客户端。
- 多个客户端可以同时使用 TCP 和 UDP。
- 连接断开后，所有相关资源能够及时释放。
- 事件循环替换线程模型后，阶段 1～5 的协议和媒体测试仍然通过。

### 阶段 7：基本 RTCP、保活和运行时管理

**难度：高。**

**工作内容：**

- 识别 TCP interleaved 中的 RTP 和 RTCP 通道。
- 支持 UDP RTCP 端口。
- 生成基本 RTCP Sender Report，并维护 RTP 时间戳与 NTP 时间的对应关系。
- 在 `PLAY` 响应中提供必要的 `RTP-Info` 信息。
- 处理 `GET_PARAMETER` 或其他保活请求。
- 解析和应用 `Session: timeout`。
- 增加客户端超时、断开、服务器停止和异常发送的清理逻辑。
- 明确文件结束、循环播放和单次播放策略。
- 为未来的真实媒体时钟、直播源和网络输入保留时间戳接口。

**验收标准：**

- Wireshark 能够识别 RTP 和 RTCP。
- 播放器长时间运行不会因为 Session 过期异常退出。
- 客户端断开、服务器停止和文件结束时资源都能释放。
- RTCP 中的 SSRC、RTP 时间戳和媒体轨道能够对应。

## 六、建议的代码演进顺序

```text
阶段 0：恢复构建和单客户端回归基线
阶段 1：增加 RtspServerConnection 和连接生命周期
阶段 2：建立 Transport、RtpSender 和统一出站队列
阶段 3：增加共享 H264MediaSource 和 MediaSession
阶段 5（优先）：增加 TCP 多轨道 SDP、AAC 和统一媒体时间线
阶段 4（延后）：增加 UDP Socket 和按轨道的 UDP Transport
阶段 6：将线程模型替换为非阻塞事件循环
阶段 7：增加 RTCP、RTP-Info、保活、超时和完整清理
```

不建议一开始同时实现多客户端、UDP、AAC 和事件循环。第一项实际交付应当是：

```text
Windows
H.264
TCP interleaved
两个或多个客户端
客户端独立 RTSP Session
每个客户端独立 RTP 状态
统一出站队列
共享一个 H264FileReader
按关键帧允许新客户端加入
```

完成这个版本后，项目具备“一个媒体源向多个 RTSP 客户端分发”的核心能力。之后增加 UDP 和 AAC 时，应扩展已有的 Transport、TrackState、RtpSender 和媒体时间线接口，而不是重新设计客户端生命周期。

## 七、主要风险和对应措施

### 1. RTSP Session 和媒体 Session 混用

**风险：** 把客户端状态和资源共享状态放进同一个对象，导致客户端互相影响。

**措施：** `RtspServerSession` 只保存客户端 RTSP 状态；`MediaSession` 只保存资源级媒体状态；通过连接管理器或订阅令牌关联两者。

### 2. 慢客户端阻塞

**风险：** 多个阻塞式 `send_all()` 依次发送时，一个慢客户端拖住所有客户端。

**措施：** 每个连接拥有有界出站队列，由单一发送者负责写 Socket；队列溢出时断开或跳到下一个关键帧。

### 3. 多线程同时写同一个 TCP Socket

**风险：** RTSP 响应、RTP 和 RTCP 的字节互相交错，客户端无法解析。

**措施：** 所有输出统一进入同一个有序队列；Socket 只由一个发送者写入。

### 4. 新客户端无法立即解码

**风险：** 新客户端从 P/B 帧开始接收，缺少 SPS/PPS 或参考帧。

**措施：** 缓存 SPS/PPS，增加 IDR 标记，新客户端从下一个 IDR 开始；测试文件必须包含可用的关键帧。

### 5. 媒体源生命周期不清晰

**风险：** 媒体源在没有客户端时提前消耗文件，或者文件结束后新客户端订阅到无效状态。

**措施：** 明确第一个 `PLAY` 启动媒体源、最后一个订阅者退出后的处理方式和 EOF 策略。

### 6. Transport 状态混用

**风险：** 视频和音频共用一个 Transport，导致通道或 UDP 目标端口混乱。

**措施：** Transport 按客户端和轨道分别保存；每条轨道拥有独立 `RtpSender`。

### 7. 音视频时间戳混用

**风险：** 使用视频的 90 kHz 时间戳发送 AAC，产生音视频漂移。

**措施：** 每条轨道使用自己的时钟；通过 PTS 和统一 Scheduler 进行跨轨道调度。

### 8. 线程模型和事件循环重复建设

**风险：** 阶段 1～3把逻辑写死在线程函数中，阶段 6需要大范围重写。

**措施：** 早期就抽象 `Connection`、`OutboundQueue`、`Transport` 和 `Scheduler`，阶段 6只替换调度器。

## 八、现有模块的处理原则

以下模块可以继续作为协议和媒体基础，不需要推倒重写：

- `src/rtsp/rtsp_message.*`
- `src/rtsp/rtsp_stream_parser.*`
- `src/rtsp/rtsp_server_session.*`
- `src/rtp/rtp_packet.*`
- `src/media/h264_packetizer.*`
- `src/media/h264_depacketizer.*`
- `src/media/h264_file_reader.*`
- `src/media/h264_file_writer.*`

这些模块需要扩展接口或调整职责：

- `RtspServerSession`：从单轨道、单 Transport 状态扩展为多轨道状态，并提供明确的会话动作或订阅事件。
- `SessionDescription`：从单个 `video` 字段扩展为轨道集合，同时维护控制 URL 和轨道查找。
- `H264FileReader`：增加关键帧、PTS、持续时间和重置或循环播放能力。
- `RtpPacket`：继续作为通用 RTP 公共头，不让媒体源直接依赖具体 Socket。
- `TcpSocket`：后续增加非阻塞、写就绪和 `WSAEWOULDBLOCK` 支持，但保留现有阻塞式测试。

需要重点新增或重构的是：

- `RtspServer`。
- `RtspServerConnection`。
- `MediaSessionRegistry`。
- `MediaSession`。
- `MediaSource`。
- `TrackState`。
- `RtpSender`。
- `MediaTransport`。
- `OutboundQueue`。
- `UdpSocket`。
- 多轨道 `SessionDescription`。
- 媒体调度器和时间戳接口。

## 九、阶段交付检查表

每完成一个阶段，都应检查以下内容：

- 旧的单元测试是否仍然通过。
- 是否增加了对应阶段的最小集成测试。
- 是否明确了对象所有权和连接关闭路径。
- 是否限制了队列、缓存和客户端资源的最大规模。
- 是否验证了一个客户端异常退出时其他客户端仍能工作。
- 是否验证了文件结束和服务器停止时资源能够释放。
- 是否记录了当前阶段不支持的 RTSP 方法、Transport 和播放策略。

路线完成的标准不是“所有类都已创建”，而是每个阶段都能通过明确的行为验收，并且下一阶段可以复用上一阶段已经稳定的接口。
