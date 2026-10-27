# 阶段 7：基本 RTCP、保活和运行时管理

本阶段完成 TCP interleaved 版本，继续使用阶段 6 的单线程 select 事件循环。UDP Transport 尚未实现，UDP RTCP 跟随阶段 4 补齐。

## 1. 架构

```text
RtspServer：一个事件循环
├── MediaSession：共享媒体进度、MediaClock、EOF
└── RtspServerConnection：每客户端一个
    ├── RtspServerSession：请求校验、Session、GET_PARAMETER
    ├── 会话期限、发送无进展期限、绝对排空期限
    ├── MediaSubscription
    │   ├── 视频：RtpSender、实际发送计数、RTCP 定时与 RR 反馈
    │   └── 音频：RtpSender、实际发送计数、RTCP 定时与 RR 反馈
    └── OutboundQueue → on_writable → TCP
```

新增三个小模块：

| 文件 | 职责 |
| --- | --- |
| `src/rtp/rtcp_packet.h/.cpp` | SR、SDES/CNAME、BYE 编码，RTCP compound 解析和 RR 报告块校验 |
| `src/media/media_clock.h/.cpp` | 建立单调时间与 NTP 的映射，按轨道时钟换算 RTP 时间 |
| `src/rtsp/runtime_options.h` | 保存 Session、RTCP、控制空闲、发送无进展和排空期限 |

协议模块不读写 Socket；没有新增线程、回调框架或通用定时器管理器。

## 2. PLAY 与 RTP-Info

```text
SETUP → 保存各轨道通道，响应 Session: id;timeout=60
PLAY  → 为已 SETUP 的轨道建立 Sender
      → 响应 RTP-Info: url=轨道URL;seq=首包序列号,...
      → 成功响应先入队
      → 有视频时等待下一 IDR，再投递媒体
```

`RTP-Info` 的 `seq` 是该轨道实际将使用的第一个序列号；H.264 参数集也属于这些 RTP 包。当前序列号从 100 开始，各客户端各轨道独立递增并允许 16 位回绕。

当前等待 IDR 后才确定订阅媒体起点，因此省略可选的 `rtptime`，不提前填入不准确的 0。音频首包保留相对视频 IDR 的时间偏移。未 SETUP 的轨道不出现在 RTP-Info 中。

## 3. RTCP 发送与统计

每个轨道在首次 RTP 完整发送后尽早发送首份 SR，之后默认每 5 秒发送一次 `SR + SDES(CNAME)`。正常结束追加 `BYE`。这是固定周期的基本实现，不包含完整 RFC 3550 带宽分配、随机间隔和成员数重考虑算法。

统计数据来自 `RtspServerConnection::on_writable()`，而不是 RTP 入队：

```text
RtpSender 入队：携带 SSRC 和 RTP 负载字节数
    → 部分发送：不计数
    → 整个 interleaved RTP 帧写完：包数 +1，负载字节数累加

RTCP 到期：先入队一条固定大小的报告占位
    → 前面的 RTP 完整发出
    → 报告成为当前消息时：快照计数和时间，替换占位字节
    → 按普通消息部分发送和续传
```

RTCP 占位与最终消息大小严格相等，仍计入队列上限。报告一旦开始发送不再修改；慢客户端不会积累重复的周期报告。最终报告在已有 RTP 后面生成，包含这些 RTP 的实际发送统计。

SR 中的 packet count 和 octet count 都是 32 位字段，允许自然回绕；octet count 只计算 RTP 负载，不包含 RTP 公共头和 TCP interleaved 头。这里的“发送完成”表示 Winsock 已接受完整帧，并不代表对端应用已经处理它。

## 4. 音视频时间对应

`MediaSession` 建立一次单调时钟与墙钟的锚点，后续 NTP 由单调时间推算，避免操作系统校时导致报告突然跳变。

每条轨道首个有效 AU 提供：

```text
该 AU 的 RTP 时间戳
该 AU 在共享调度器中的预定发送时刻
该轨道的 clock_rate
```

发送 SR 时以同一个时刻计算：

```text
NTP = MediaClock.ntp_at(报告快照时刻)
RTP = 首个 AU 时间戳 + (报告快照时刻 - AU 预定时刻) × clock_rate
```

视频使用 90000 Hz，AAC 使用采样率。不能将当前 NTP 与最后一个 RTP 包的时间戳直接配对。RTP 字段按 32 位回绕，换算时先拆秒再处理小数，避免长时间运行时纳秒乘法溢出。

当前 H.264 仍为固定 25 fps、解码顺序时间；没有恢复真实 PTS/DTS。RTCP 提供的是现有简化媒体时间线的对应关系，不能据此宣称 B 帧显示同步或任意时长播放已经完成验收。

## 5. 接收 RTCP 和保活

入站 interleaved 帧先按 SETUP 的通道定位轨道：

- RTCP 通道：完整校验 compound 的版本、长度、padding 和已支持的包结构。
- RTP 通道：当前服务器只发送媒体，收到反向 RTP 报错关闭。
- 未协商通道或 SETUP 前的帧：报错关闭。

RR 报告块只有在同一条轨道的 RTCP 通道上引用该轨道 Sender 的 SSRC，且已发送过 RTP 时，才保存反馈并刷新会话期限。保存 fraction lost、累计丢包、最高序号、jitter、LSR 和 DLSR；暂不据此调码率或重传。未知 RTCP 类型按公共长度跳过，不作为保活；无关 SSRC 和空 RR 也不续期。

服务端首次 SETUP 成功后启动 Session 期限。后续成功 SETUP、携带正确 Session 的有效请求，以及匹配的 RR 都能续期。错误请求、半条消息、任意收到的字节、服务端持续发送 RTP 均不能续期。客户端请求中的 `timeout` 不改变服务端策略。

支持空消息体的 `GET_PARAMETER`；非空消息体返回 451，缺失或错误 Session 返回 454。携带 Session 的 OPTIONS 也可以保活。Public 头包含 GET_PARAMETER。

自带客户端解析 SETUP 响应的 Session timeout；未声明时默认 60 秒。在约一半期限发送 GET_PARAMETER，服务端未声明支持该方法时使用 OPTIONS。媒体接收循环同时分流 RTCP 和保活响应，等待媒体期间也能触发保活，无需另开线程。保活响应有独立等待期限。

## 6. 超时与收尾

`RuntimeOptions` 默认值：

| 配置 | 默认值 | 含义 |
| --- | --- | --- |
| `session_timeout_seconds` | 60 秒 | SETUP 后等待有效会话活动的期限 |
| `rtcp_interval` | 5 秒 | 周期 Sender Report 间隔 |
| `control_idle_timeout` | 60 秒 | SETUP 前 TCP 接收空闲期限 |
| `send_stall_timeout` | 2 秒 | 有输出时连续无发送进展期限 |
| `drain_timeout` | 2 秒 | 进入 Draining 后的绝对总期限 |

可以通过 RtspServer / RtspServerConnection 的构造参数修改，命令行暂不增加配置参数。事件循环把连接和 RTCP 的下一期限与媒体期限一起纳入 select 等待计算。

```text
Open
 ├─ TEARDOWN / EOF / 接收半关闭 / Session 到期 / stop → Draining
 └─ 格式错误 / 网络错误 / 输出队列失败 → Closed

Draining
 ├─ 停止投递媒体和周期 RTCP
 ├─ 已有 RTP → 最终 SR + SDES + BYE → 必要的 RTSP 响应
 ├─ 关闭队列入口，继续按事件发送
 └─ 排空、无进展超时或绝对期限到达 → Closed
```

正常结束时只有发送过 RTP 的轨道才发送最终报告；从未等到 IDR 的订阅直接结束。异常或队列已满时不保证 BYE 送达，优先释放资源。收尾幂等，重复进入不会重复发送最终 BYE。

`RtspServer::stop()` 只置原子标记。运行线程停止接受新连接、停止媒体源，然后通过同一个事件循环有限排空连接。析构前仍须等待运行线程退出。控制台程序接入 Ctrl+C / Ctrl+Break 停止；控制处理线程只置标记，Socket 操作都留在运行线程。

## 7. 文件策略

沿用共享进度和单次播放：首个 PLAY 启动；无人订阅时暂停；新视频订阅等待 IDR；某轨道 EOF 不影响另一轨道；全部 EOF 后结束订阅并排空。已结束资源的新 PLAY 返回 455，重播需要重启服务器。

没有加入循环播放、seek、直播源、网络输入、UDP 或完整拥塞控制。未来媒体源可继续提供 PTS、clock_rate 和调度时刻，复用当前 RTCP 映射接口。

## 8. 验证与阅读顺序

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

`runtime_tests` 覆盖固定 RTCP 网络字节、截断和错误 padding、RR 报告块、NTP epoch 和小数、RTP 回绕、timeout 参数、有效续期与错误请求不续期、实际完成发送计数、周期/最终报告、双轨道 SSRC/通道/时钟对应、部分发送后的 TEARDOWN、服务器停止和自带客户端短期限保活。另覆盖晚加入客户端等待约 3 秒 IDR、Session 期限仅 1 秒时仍能保活并从正确关键帧开始接收。

2026-10-27 外部验证：66 秒无 B 帧 H.264 + 48 kHz 双声道 AAC-LC，FFmpeg 与自带客户端同时连接。FFmpeg 收到 1650 帧，逐帧解码哈希与源文件一致；音频解码 PCM 共 12677120 字节，与源文件完全一致。自带客户端晚加入后收到正确的 1625 帧后半段。服务器处理 4 次 GET_PARAMETER，没有会话过期，FFmpeg 无错误输出。结果位于本机 `build/stage7_verification/result.json`，媒体不加入 Git。此验证跨越默认 60 秒 Session 期限，不代表任意时长同步验收；本次未使用 Wireshark 实际抓包。

MSVC Debug 构建通过，13 组 CTest 全部通过（21.67 秒），包含原有媒体回归和新增运行时测试。

阅读顺序：`RuntimeOptions` → `RtspServerSession::handle()` → `RtspServerConnection::handle_item()` / `check_timeout()` → `MediaSubscription::tick_rtcp()` / `prepare_rtcp()` → `on_writable()` → `rtcp_packet.cpp` / `media_clock.cpp` → `RtspClient::receive_media()`。
