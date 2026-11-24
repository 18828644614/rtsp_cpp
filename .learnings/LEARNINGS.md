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

## [LRN-20261118-AAC] best_practice

**Logged**: 2026-11-18
**Priority**: high
**Status**: resolved
**Area**: backend

### Summary
AAC 编码和 RTP 互通必须分别保留包时间、核对 padding，并允许每轨独立的非零 RTP 原点。

### Details
本机 AAC initial_padding 为 1024，首包 PTS 为 -1024。44117 个 44.1 kHz 输入采样转换为 48019 个有效采样，补 109 后提交 48128，外部解码产生 49152=1024+48019+109。脉冲从 0.25 秒映射到解码位置 13024。FFmpeg 把 base_timestamp=0 用作未初始化标记，首次 SR 可能使零起点产生小幅偏移；网络音频采用非零起点后外部 PTS 连续。

### Suggested Action
音频 AU 使用有符号 PTS，输出直接保留编码包 PTS/duration；FIFO 和重采样器分别排空，记录手动补零和编码器填充。比较媒体时间或验证 SR 时，先去掉每轨 RTP 原点。保留外部解码、脉冲及 RTP 回绕测试。

### Metadata
- Source: error
- Related Files: src/media/aac_encoder.cpp, src/media/media_time.h, tests/aac_encoder_tests.cpp, tests/verify_audio_encoding.py
- Tags: aac, padding, pts, rtcp, ffmpeg
## [LRN-20261119-AUD] best_practice

**Logged**: 2026-11-19
**Priority**: high
**Status**: pending
**Area**: tests

### Summary
Synthetic audio decode tests do not establish continuity with real DirectShow capture buffers.

### Details
The Realtek microphone delivered 500 ms PCM blocks with its default buffer.
MicrophoneAudioSource retains only nine AAC AUs (192 ms), and MediaSession drops
audio older than its default 150 ms send buffer plus 50 ms lateness allowance.
A four-second audio-source probe observed 105 queue drops with the default
capture buffer, versus zero with an explicit 50 ms capture buffer. The latter
also had zero observed PTS gaps and zero AUs older than 200 ms when read.
This validates an audio-source defect, but does not establish full playback QA.

### Suggested Action
Set and validate capture block duration, cover bursty PCM input in regression
tests, and count audio queue drops separately from scheduler late drops.

### Metadata
- Source: user_feedback
- Related Files: src/media/microphone_capture.h, src/media/microphone_audio_source.h, src/media/media_session.cpp, .verify/audio-diagnostics/README.md
- Tags: audio, dshow, continuity, buffering, hardware-validation

### Follow-up (2026-11-24)

The user's running Debug server reproduced the problem: a six-second companion
player session decoded 67 AAC frames, recorded 10 PCM discontinuities, and
displayed only 43 of 136 decoded video frames. This player uses audio position
to schedule video, so capture gaps can interrupt both media tracks at playback.
The live diagnostic used SDL dummy output and did not save captured media.

A fresh four-second microphone-source comparison observed 128 queue drops and
115 missing AUs with the device default, versus 188 received AUs, zero queue
drops, zero missing AUs and zero AUs older than 200 ms with a 50 ms buffer.
MicrophoneConfig now requests 50 ms by default; explicit zero still selects
the device default. Release server/player builds and the five relevant server
tests passed. The currently running Debug executable remains unchanged until
the user stops it and starts the rebuilt Release server. Full physical playback
quality still requires verification after that restart.

---
