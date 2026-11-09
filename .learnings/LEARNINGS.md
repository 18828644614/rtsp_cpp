# 学习记录（Learnings）

## [LRN-20261109-CVS] best_practice

**Logged**: 2026-11-09
**Priority**: medium
**Status**: resolved
**Area**: backend

### 摘要（Summary）
实时源应由消费者锁定 EOF；Windows 设备资源的打开和关闭应保留同线程生命周期。

### 详情（Details）
`read(false)` 后生产者可能推入最后一帧并关闭，直接从生产者完成标记实现 `ended()` 会丢失最后一帧。应由带锁队列读操作确认“关闭且空”，再锁定消费者终态。DirectShow 设备适配器延迟到工作线程打开，并在同线程关闭；额外使用外部原子停止标记，避免打开初始化覆盖并发停止。

### 建议行动（Suggested Action）
新增实时输入时沿用队列原子状态读取和输入线程生命周期契约，通过可控制的无硬件输入验证等待、排空、错误与停止。

### 元数据（Metadata）
- Source: conversation
- Related Files: src/media/camera_video_source.cpp, src/media/video_frame_queue.cpp, src/media/camera_capture_input.cpp
- Tags: live-source, eof, concurrency, lifecycle

## [LRN-20261021-001] correction

**Logged**: 2026-10-21
**Priority**: high
**Status**: resolved
**Area**: backend

### 摘要（Summary）
初学者项目应先突出 TCP 调用顺序，避免同时引入移动语义、静态工厂、友元和智能指针删除器。

### 详情（Details）
用户要求代码简单清晰，但第一版封装需要先理解较多 C++ 所有权机制。用户在学习中明确要求简化。

### 建议行动（Suggested Action）
先创建对象，再调用普通成员函数。保留正确收发和自动关闭；接受连接通过引用输出，测试使用顺序调用减少线程知识负担。

### 元数据（Metadata）
- Source: user_feedback
- Related Files: src/net/tcp_socket.h, src/net/tcp_socket.cpp, tests/tcp_tests.cpp
- Tags: teaching, simplicity, tcp

### 解决情况（Resolution）
已改为普通成员函数和引用输出，只保留一个 Socket 类；端口解析及测试同步简化，编译和回显验证通过。

## [LRN-20261026-001] ffmpeg-zero-range-shared-progress

**Logged**: 2026-10-26T20:45:00+08:00
**Priority**: high
**Status**: resolved
**Area**: backend

### 摘要（Summary）
FFmpeg/ffplay 会在每次 PLAY 发送 `Range: npt=0.000-`；对共享媒体服务器而言，晚加入客户端必须把这个零起点 Range 当作加入当前共享进度，而不能当作重新回放请求。

### 详情（Details）
RTSP 会话层已经正确接受零起点 Range，但连接层把任意非空 Range 传成 `MediaSession::subscribe(..., from_start=true)`。第一个客户端可以播放，第二个 ffplay 因共享进度已启动收到 `457 Invalid Range`，导致外部播放器互通失败。

### 建议行动（Suggested Action）
在 RTSP 连接层仅把已通过会话校验的 PLAY 交给共享订阅；零起点 Range 传 `from_start=false`，非零 Range 继续由会话层返回 457。保留回归测试覆盖两个 TCP 连接的晚加入场景。

### 元数据（Metadata）
- Source: error
- Related Files: src/rtsp/rtsp_server_connection.cpp, tests/multitrack_tests.cpp, README.md
- Tags: rtsp, ffplay, ffmpeg, range, multi-client

### 解决情况（Resolution）
- **Resolved**: 2026-10-26T20:45:00+08:00
- **Commit/PR**: working tree
- **Notes**: 连接层已将合法零起点 Range 视为共享进度加入；新增 TCP 晚加入回归测试，全部 11 个 CTest 通过。
