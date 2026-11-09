# 摄像头阶段 4：CameraVideoSource 实现说明

2026-11-09：实现采集/编码工作线程、有界队列和实时媒体调度。没有实体摄像头时，使用生成的原始帧运行相同的编码、队列和媒体分发链路。

## 数据流与代码阅读顺序

```text
CameraFrameInput（真实设备适配器 / 测试生成帧）
    → CameraVideoSource 工作线程
    → H264Encoder：原始帧 → H264AccessUnit
    → VideoFrameQueue：线程安全、限制帧数和字节数
    → MediaSession::tick：非阻塞读取、PTS 调度、订阅分发
    → H264Packetizer → 各订阅者的 RTP/OutboundQueue
```

| 文件 | 职责 |
| --- | --- |
| `src/media/video_frame_queue.h/.cpp` | 完整 AU 的有界队列、IDR 恢复、结束与异常传递 |
| `src/media/camera_video_source.h/.cpp` | 输入接口、采集/编码线程、首帧启动握手、VideoSource 实现 |
| `src/media/camera_capture_input.h/.cpp` | 将阶段 2 的 CameraCapture 接入工作线程 |
| `src/media/video_source.h` | 增加默认的 is_live、resume、stop 生命周期钩子 |
| `src/media/media_session.cpp` | 等待重试、空闲后恢复、停止生产资源 |
| `tests/video_frame_queue_tests.cpp` | 容量、依赖链、结束、异常与生产/消费并发 |
| `tests/camera_video_source_tests.cpp` | 生成帧采集/编码、源状态、媒体交付与 RTP 实际解码 |
| `tests/media_session_tests.cpp` | 不依赖 FFmpeg 的等待/结束/失败与双轨道等待回归 |

## 源状态与启动

`CameraVideoSource` 构造时启动一个工作线程。线程创建编码器，读取输入并编码；构造线程最多等待 `startup_timeout`（默认 5 秒）取得首个成功入队的 IDR 和 SPS/PPS。由编码器真实参数生成 `track().fmtp`，只发布一次，随后保持不变。这样符合现有 `VideoSource` 的“构造完成后即可取轨道描述”契约。

| read/ended 行为 | 含义 | MediaSession 的处理 |
| --- | --- | --- |
| `read() == true` | 已取到完整编码帧 | 按 90 kHz PTS 调度并分发 |
| `read() == false && !ended()` | 当前队列为空，输入仍可继续 | 设置 5 ms 重试期限，返回网络事件循环 |
| `read() == false && ended()` | 生产者已正常结束，队列也已排空 | 所有轨道结束后执行正常 finish |
| `read()` 抛出异常 | 生产者采集/编码失败，已缓存帧已经排空 | MediaSession 执行失败收尾 |

正常输入结束时先 `H264Encoder::flush()`，将剩余 AU 入队，再关闭队列。失败时保存 `exception_ptr`，消费者先排空有效缓存，再收到原异常。暂时无帧不会触发 BYE、关闭队列或结束订阅。

`ended()` 由消费者在一次确定的“关闭且空”的读取后锁定。不能直接用生产者完成标记实现：消费者读取返回 Waiting 后，生产者可能刚好推入最后一帧并关闭，随后直接查询生产者状态会误判 EOF、丢失最后一帧。当前实现通过一次带锁的队列读取确定状态，避免这个竞态。

若音频已有未来的待发帧而视频还在等待，下一次唤醒取“音频期限”和“5 ms 视频重试”中较早的时刻。音频文件 EOF 不会结束仍在等待的实时视频。

## 容量、丢帧与空闲

默认最多保留 60 个 AU、8 MiB NALU 负载；容器和对象开销由最大帧数间接限制。生产者不等待空位，消费者不等待新帧，互斥锁只用于短期队列操作。

超过任一限制时清除旧队列，进入等待 IDR 状态。新到帧为 IDR 且能放入字节预算时立即恢复；否则丢弃，直到新的 IDR。不能只丢一个旧 P 帧、继续发送其后的 P 帧，因为它们可能引用已丢失的帧。单个 IDR 超过字节上限时，实时源明确失败，避免永久等待一个永远无法入队的关键帧；可调整队列容量或编码参数。

无人订阅时 `MediaSession` 不消费源、也不设置媒体期限；生产者持续采集，积压仍受队列双重限制。重新出现第一个订阅者时，通过 `resume()` 保留队列中最新 IDR 及其连续后继；若没有 IDR，则清空并等待新的 IDR。同时丢弃 `MediaSession` 预读的旧视频帧，重建调度时钟。已有客户端仍使用原有的独立出站队列。

`queue_stats()` 提供当前帧数、字节数、累计丢弃数、等待 IDR 和关闭状态，便于验证；阶段 6 的完整运行日志仍待实现。

## 生命周期与设备适配

`CameraFrameInput` 只有读取、并发停止通知与关闭操作。`read()` 在工作线程调用，可返回 FrameReady、Waiting、Ended，不可恢复错误通过异常报告。实现必须遵守读取超时，并响应 `request_stop()`。测试输入通过条件变量等待生成帧许可；真实适配器使用既有采集接口。

`make_camera_capture_input()` 只保存配置，在工作线程第一次读取时打开设备，结束时在同一线程关闭，保持 DirectShow/COM 的创建与释放线程一致。`CameraCapture::open()` 支持可选的外部原子停止标记，避免打开过程中内部状态初始化覆盖已经发出的停止请求。

`MediaSession::stop()`、正常结束和失败收尾均调用源的 `stop()`；源先设置停止标记并通知输入，等待线程退出及输入/编码器关闭，再丢弃残留队列。重复停止安全，析构也执行该流程。网络读帧和 Waiting 调度不等待采集；停止的完成时间依赖输入实现遵守停止契约，设备驱动的实际响应仍须有硬件时验证。

## 构建与使用

保持默认构建无需 FFmpeg。只开启编码即可构建 `camera_video_source` 库及生成帧专项测试：

```powershell
cmake -S . -B build-encoder -DENABLE_H264_ENCODING=ON `
  -DFFMPEG_ROOT="E:/MarkDown/rtsp-cpp/.verify/stage3-sdk/ffmpeg-master-latest-win64-gpl-shared"
cmake --build build-encoder --config Debug --parallel 4
ctest --test-dir build-encoder -C Debug --output-on-failure
```

这里的 SDK 路径为本机既有 SDK，其他机器替换为自己的 FFmpeg shared SDK 路径。SDK 要提供 libx264、avcodec、avutil 和 swscale。

同时开启采集后，库提供真实设备工厂：

```powershell
cmake -S . -B build-camera-encoder -DENABLE_CAMERA_CAPTURE=ON `
  -DENABLE_H264_ENCODING=ON `
  -DFFMPEG_ROOT="E:/MarkDown/rtsp-cpp/.verify/stage3-sdk/ffmpeg-master-latest-win64-gpl-shared"
cmake --build build-camera-encoder --config Debug --parallel 4
ctest --test-dir build-camera-encoder -C Debug --output-on-failure
```

后续入口程序链接 `camera_video_source`，在启动事件循环前创建源并交给会话即可：

```cpp
#include "media/camera_capture_input.h"
#include "media/media_session.h"

rtsp::CameraConfig capture;
capture.device_name = "Integrated Camera";
rtsp::CameraVideoSourceConfig config;
config.encoder.width = capture.width;
config.encoder.height = capture.height;
config.encoder.fps_num = capture.fps_num;
config.encoder.fps_den = capture.fps_den;
config.encoder.pixel_format = capture.pixel_format;
auto source = std::make_unique<rtsp::CameraVideoSource>(
    rtsp::make_camera_capture_input(capture), config);
auto media = std::make_shared<rtsp::MediaSession>(std::move(source));
```

自定义输入只需实现 `CameraFrameInput`，返回 `CameraFrame`。专项测试的 `GeneratedInput` 演示了无摄像头用法，真正使用 libx264 编码，随后进入同一个 `MediaSession`。

本阶段交付到 `MediaSession`；服务器摄像头命令行、VLC/ffplay 连接和多客户端实机播放仍属于阶段 5。首个 IDR 的轨道配置准备已经完成，但尚未执行真实设备的 SDP/RTSP 播放验收。

## 无硬件验证

专项验证覆盖：首次 P 帧拒绝、帧数/字节边界、超大 IDR、丢帧后 IDR 恢复、最新关键帧恢复、生产消费并发、缓存排空后 EOF/异常、启动超时/空输入/输入失败、阻塞输入停止与重复停止、空闲订阅、清除预读旧帧、音频未来帧期间的视频重试和独立 EOF。

生成原始图像实际经过 libx264、线程队列和 MediaSession，输出 RTP 经现有解包器重组后交给 FFmpeg H.264 解码器，正常结束的 6 帧全部解码，故障前缓存的 1 帧仍可解码。真实设备适配器另用预停止和非法配置验证工作线程的启动失败传播，不打开任何设备。

MSVC Debug 最终构建与 CTest 全部通过：默认构建 14/14（21.24 秒）、仅编码 19/19（22.45 秒）、采集加编码 22/22（22.02 秒）。原有文件传输集成测试在三种配置中均通过，`git diff --check` 通过。

摄像头打开、物理拔出、持续实时采集和 VLC/ffplay 现场播放待具备设备后验收。
