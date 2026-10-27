# 阶段 6：事件循环与非阻塞网络

一个 `select` 循环负责所有服务器 Socket 和媒体定时任务。只新增一个网络类 `EventLoop`，协议状态机、SDP、RTP、H.264/AAC 封包规则继续复用。

阶段 7 已在此框架接入 RTCP、Session 保活和有限收尾，详见 [RTCP 与运行时实现说明](rtcp_runtime_implementation.md)。

## 1. 类的分工

```text
RtspServer::run()                     ← 唯一的服务器运行线程
├── EventLoop                        ← 等待多个 Socket 可读/可写
├── listener                         ← 非阻塞接受连接
├── MediaSession                     ← 按期限 tick，不创建媒体线程
│   └── MediaSubscription / RtpSender ← 生成数据并入队
└── 多个 RtspServerConnection
    ├── TcpSocket                    ← 非阻塞收发
    ├── RtspStreamParser             ← 跨事件保留半包
    ├── RtspServerSession            ← 跨事件保留协议状态
    ├── MediaSubscription            ← 管理订阅生命周期
    ├── OutboundQueue                ← 保存有序输出
    └── sending_ / send_offset_       ← 保存当前消息和发送偏移
```

| 类 | 关键接口 | 职责 |
| --- | --- | --- |
| `EventLoop` | `clear()`、`watch()`、`wait(timeout_ms)` | 只管理一轮 select 的关注集合与就绪结果 |
| `RtspServer` | `run()`、`stop()` | 管理对象，计算等待时间，分派事件，回收连接 |
| `RtspServerConnection` | `on_readable()`、`on_writable()`、`check_timeout()`、`close()` | 处理一个连接，保存所有跨事件状态 |
| `TcpSocket` | `set_nonblocking()`、`try_accept()`、`receive_some()`、`send_some()` | 非阻塞操作与错误映射 |
| `MediaSession` | `tick(now)`、`next_deadline()` | 推进到期媒体，提供下一次期限 |
| `OutboundQueue` | `try_pop()`、`take_for_send()`、`complete_send()` | 非阻塞取出，约束队列与在途消息 |

`MediaSubscription` 和 `OutboundQueue` 的所有访问均由所属事件循环线程串行执行，不提供线程同步，也没有阻塞 `pop()` 接口。测试显式推进事件循环后用 `try_pop()` 取出消息。

`wait_readable()` 内部也使用 EventLoop，只关注一个 Socket 的读事件。自带客户端继续使用阻塞的 `connect()`、`receive()` 和 `send_all()`。

## 2. 每轮如何运行

```text
1. media.tick(now)：处理到期数据，向各连接入队
2. 检查连接超时、订阅失败和正常结束
3. 移除已经 Closed 的连接
4. 关注监听 Socket 可读
5. 关注 Open 连接可读，有输出的连接可写
6. 计算 timeout = min(下一媒体/连接/RTCP 期限距现在的毫秒数, 50 ms)
7. EventLoop.wait(timeout)
8. 监听可读 → 每轮最多接受 16 个连接
9. 连接可读 → on_readable()
10. 连接可写 → on_writable()
11. 移除本轮结束的连接，继续下一轮
```

没有媒体任务时最多等待 50 ms；超时检查也因此最多约有 50 ms 调度延迟。`stop()` 只设置原子标记，实际 Socket 清理由循环线程完成。使用者若从其他线程启动 `run()`，必须先 stop、等待该运行线程退出，再销毁服务器。

循环不使用 `sleep` 或条件变量等待媒体；select 的超时返回本身就是简单的定时驱动。当前没有通用定时器管理器和回调框架。

## 3. 非阻塞结果

监听和接受到的客户端都设置 `FIONBIO`。`try_accept()` 在没有连接时返回 false；接收和发送返回 `IoResult`：

| 状态 | 含义 | 处理 |
| --- | --- | --- |
| `Done` | 完成了一部分操作，`bytes` 为实际字节数 | 更新进度 |
| `WouldBlock` | 当前无法继续 | 返回事件循环，等待下一轮就绪 |
| `Eof` | recv 返回 0，对端结束发送 | 检查解析器完整性，进入排空关闭 |

其他错误抛出异常，由对应连接处理并关闭该连接。事件循环不把单连接错误扩大为服务器退出。

可读不保证消息完整，因此仍需要流解析器；可写不保证整条消息能发送完，因此必须保存偏移。

## 4. 发送进度与输出上限

```text
队列消息 A（1000 字节）
  → take_for_send：取出 A，仍占队列字节/消息额度
  → send_some：发送 300 字节，偏移变为 300
  → WouldBlock：返回循环，保留 A 和偏移
  → 下一轮可写：从偏移 300 开始
  → 全部发送完成：complete_send 释放额度
  → 再取下一条消息 B
```

RTSP 响应、RTP 和 RTCP 均经过同一条有序队列。默认每连接上限为 4 MiB，包含当前在途消息；从队列移出但尚未发送完成的数据不会漏算。

仅在有待发送数据时关注可写，避免空队列连接不断唤醒 select。每次可读/可写处理最多收发 64 KiB，避免一个繁忙连接占满整个循环。

## 5. 媒体定时推进

原 `MediaSession::run()` 的预读视频、预读音频、时钟原点和下一期限已改为成员变量。

`tick(now)` 清理失效订阅，预读每轨道一帧，按 `pts / clock_rate` 选择时间更早的帧：到期则分发，未到期则记录期限并返回。每轮最多推进 32 个 AU，积压媒体会分轮处理。

AU 通过输出参数读取，Packetizer 接收常量引用并按值返回 `RtpPayloadUnit`；视频、音频待发负载各由一个 `optional<RtpPayloadUnit>` 持有。同步分发时使用常量引用，每帧仍只封包一次，未到期时保留负载。RTP/RTCP 直接使用 SETUP 协商的通道调用 `serialize_interleaved_frame()`，不经过额外传输包装类。

无有效订阅时暂停并清空下一期限，保留预读数据和读取位置。新订阅加入后重建单调时钟起点，避免将空闲期间的媒体集中突发发送。视频订阅仍等待 IDR；每轨道 EOF 独立处理，所有轨道结束后将最终 RTCP 入队并关闭订阅队列。

`MediaSession` 由事件循环单线程访问，外部线程不能直接调用其 tick、subscribe 或 stop；跨线程停止服务器使用 `RtspServer::stop()`。

## 6. 生命周期与超时

```text
Open
 ├─ TEARDOWN / 正常媒体 EOF / 接收半关闭 / Session 到期 / 服务器停止 → Draining
 └─ 网络错误 / 队列溢出 / 发送无进展超时 → Closed

Draining
 ├─ 输出全部发送完成 → shutdown_send → Closed
 └─ 发送无进展超时 / 绝对排空期限 / 网络错误 → Closed
```

TEARDOWN 先结束订阅并排入最终 RTCP，再排入响应；接收半关闭先处理完整消息并检查是否截断，再停止媒体、排入最终 RTCP 并排空输出。正常 EOF 同样允许发送完队列。`Closed` 对象由服务器统一移除。

- SETUP 前无接收进展 60 秒回收；SETUP 后按声明的 Session timeout 检查有效会话活动，默认 60 秒。
- 有输出时连续 2 秒无发送进展回收，EOF 排空阶段也适用。
- 队列溢出终止对应订阅，下一次循环检查关闭该连接。
- 阶段 7 已实现 Session timeout、GET_PARAMETER / Session OPTIONS 保活和基本 RTCP；Draining 另有默认 2 秒绝对期限。

## 7. 平台和范围

Windows select 的上限为 `FD_SETSIZE`，默认 64；监听占一个名额，因此默认最多 63 个客户端。达到上限后接受并关闭多余连接，已有连接继续运行。

当前仅 TCP interleaved。UDP 仍在阶段 4；阶段 7 已使用现有通道发送 RTCP SR/SDES/BYE。超时和报告周期通过 RuntimeOptions 传入；公平预算仍为教学版固定值，没有通用配置框架。

## 8. 阅读与验证

建议按下面顺序阅读：

1. [RtspServer::run](src/rtsp/rtsp_server.cpp)：整体顺序与等待时间。
2. [EventLoop](src/net/event_loop.cpp)：复用 select 的读写就绪等待。
3. [TcpSocket](src/net/tcp_socket.cpp)：非阻塞操作与 WouldBlock。
4. [RtspServerConnection](src/rtsp/rtsp_server_connection.cpp)：可读、可写和排空状态。
5. [MediaSession::tick](src/media/media_session.cpp)：到期媒体的生成与分发。
6. [OutboundQueue](src/net/outbound_queue.cpp)：在途消息额度。

验证命令：

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

`event_loop_tests` 验证非阻塞 accept/recv、select 读写事件、真实发送背压后的完整续传、半包/粘包和半关闭排空、慢客户端隔离、媒体期限与暂停恢复、32 AU 预算、连接上限与跨线程停止。媒体和多轨道测试通过测试专用驱动器在调用线程推进，不再依赖后台媒体或连接线程。

2026-10-27 验证：MSVC Debug 构建成功，全部 12 组 CTest 通过（14.26 秒），包含原有双进程媒体集成测试。当前工作不包含新的外部 FFmpeg 解码验收。
