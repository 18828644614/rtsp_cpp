# 音视频同步与实时麦克风采集实现方案

> 更新：2026-11-19。阶段 1～5 已实现，真实设备验收与阶段 6 待完成。阶段 3 的接口和验证见 [独立音频编码实现说明](audio_encoding_implementation.md)，阶段 5 的无设备验证见 [双轨实时行为实现](#阶段-5-双轨实时行为实现)。
> 目标环境：Windows；输入为摄像头与电脑麦克风；输出为 H.264 + AAC-LC 的 RTSP/TCP 双轨道。

## 1. 目标与范围

在保留现有 RTSP 会话、RTP 封包、共享媒体源和事件循环结构的基础上，补齐实时音频采集与同步。

完成后应支持：

- 摄像头和麦克风独立采集，共享媒体时间起点，保留设备实际启动时间差。
- 多个客户端共享一路采集和编码，各自维护 RTP/RTCP 状态与有界出站队列。
- 暂时缺帧、晚加入、无人观看后恢复以及短期抖动时，保持时间线连续。
- 监测并补偿长时间运行中的设备时钟漂移。
- 保留已有 H.264/AAC 文件模式和单视频模式。

第一版固定使用无 B 帧 H.264、AAC-LC、48 kHz 单声道、RTSP/TCP。采集格式允许与编码格式不同，通过重采样转换。桌面系统声音、回声消除、UDP、自动设备热重连、自研播放器和容器文件解复用不属于第一版范围。麦克风采集与系统声音采集是两种输入需求，本文只实现麦克风。

## 2. 当前代码基础

| 现有模块 | 当前能力 | 本次调整 |
| --- | --- | --- |
| `VideoSource` / `CameraVideoSource` | 非阻塞读取、采集编码线程、视频有界队列 | 注入共享时间线，保留采集时间偏移 |
| `CameraFrame` / `CameraCapture` | 设备 PTS、时间基、相对 PTS、到达单调时刻 | 增加明确的共同时间线时间字段或映射结果 |
| `H264Encoder` | 将设备时间换算为 90 kHz，禁用 B 帧 | 增加共同时间线模式，取消该模式下的独立首帧归零 |
| `AacFileReader` / `AacPacketizer` | 文件 AAC 读取与 MPEG4-GENERIC RTP 负载 | 包装文件音频源，复用封包器 |
| `MediaSession` | 比较两轨 PTS、统一调度和分发 | 接入抽象音频源，区分文件和实时调度 |
| `MediaSubscription` | 晚加入等待 IDR、共同订阅起点、每轨 RTP 状态 | 适配有符号时间与采集时刻映射 |
| `MediaClock` | 单调时钟与 NTP 映射、RTP 外推 | 复用，核对实时媒体时刻与发送时刻的区别 |
| 服务器命令行 | 文件音视频、实时摄像头 | 增加麦克风选择和同步参数 |

重要事实：

1. 阶段 1 已将 `MediaSession` 改为持有 `unique_ptr<AudioSource>`，文件读取由 `FileAudioSource` 包装；阶段 3 已接入 `MicrophoneAudioSource`。
2. `CameraCapture` 的 `relative_pts_us` 仍按本设备首帧归零；共同模式使用独立的 `capture_pts_us`，`H264Encoder` 不再减去本轨起点。
3. 阶段 3 已将媒体 AU、通用负载和订阅起点改为有符号 PTS，保留 AAC 起始填充产生的负时间；RTP 仍使用 uint32_t。
4. 已有基本 RTCP SR，无需重新实现 RTCP 协议；需要修改并验证时间锚点的语义。
5. 自带客户端目前主要接收并保存 H.264，不具备完整音视频播放验收能力。

相关源码：`src/media/media_session.h/.cpp`、`src/media/media_clock.h/.cpp`、`src/media/camera_capture.cpp`、`src/media/h264_encoder.cpp`、`src/media/aac_access_unit.h`、`src/rtp/rtp_payload.h`。

## 3. 总体结构与线程模型

```text
                  共享 CaptureTimeline / 时钟映射
                         │              │
摄像头 → CameraCapture → H264Encoder    │
                         │              │
                    视频有界队列        │
                         │              │
                         ├──────────────┼──────────────┐
                         │              │              │
麦克风 → MicrophoneCapture → 重采样 → PCM FIFO → AacEncoder
                                                    │
                                               音频有界队列
                                                    │
                         └───────────┬───────────────┘
                                     ↓
                         MediaSession::tick（事件循环）
                                     ↓
                        H264Packetizer / AacPacketizer
                                     ↓
                  各客户端 MediaSubscription / RTP / RTCP
                                     ↓
                        各客户端有界 OutboundQueue
```

- 视频工作线程负责摄像头读取和 H.264 编码。
- 音频工作线程负责麦克风读取、重采样、PCM FIFO 和 AAC 编码。第一版先合并为一个线程，避免过早增加线程间队列。
- 网络事件循环只读取已编码 AU，执行时间调度、封包、连接收发和超时处理。
- 所有设备句柄和编解码上下文在其所属工作线程创建和释放；停止通过可并发设置的标志发起。
- 不在事件循环中等待新 PCM、新视频帧或编码器输出。队列互斥锁只保护短期操作。

## 4. 同步模型：单调时钟参考 + 连续音频采样时间

### 4.1 三种时钟的职责

| 时钟 | 用途 | 约束 |
| --- | --- | --- |
| `steady_clock` | 本机采集时间线、发送期限、超时 | 作为两路共同参考 |
| 音视频设备时钟 | 描述实际采样过程 | 分别映射到共同参考，不直接比较原始值 |
| 墙钟 / NTP | RTCP 向接收端提供共同参考时间 | 复用现有单次墙钟采样映射，避免系统调时造成跳变 |

本文选择单调时钟作为发送端共同参考，音频在输出端维持连续采样序列。最终播放端仍需根据 RTP/RTCP 做同步；服务器的发送顺序本身不能保证播放器的呈现时刻。[1]

### 4.2 共同起点与启动顺序

在打开设备前创建唯一的 `T0 = steady_clock::now()`，由视频、音频生产者和 `MediaSession` 共享。第一版启动顺序：创建 T0 → 启动并等待视频源配置就绪 → 启动并等待音频源配置就绪 → 注册媒体会话。两个源即使顺序启动，也必须保留各自相对 T0 的时间偏移；生产队列应保持有界。

```text
T0 +  80 ms：首段音频的第一个采样
T0 + 200 ms：首个视频帧

48 kHz 音频首个有效采样时间：3840 ticks
90 kHz 视频首帧时间：       18000 ticks
实际启动偏移：             120 ms
```

不能把上面两条轨道各自改成从 0 开始。编码器需要明确区分原有独立时间线模式与新增共同时间线模式。

### 4.3 时间戳计算

设 `t_video` 为视频帧对应的采集时刻，`t_audio0` 为首个有效音频采样的采集时刻，`N_out` 为此前累计输出的 PCM 采样数，`Fs` 为编码采样率：

```text
video_pts = rescale(t_video - T0, 1/90000)
audio_input_pts = rescale(t_audio0 - T0, 1/Fs) + N_out
```

`N_out` 以重采样器实际输出、送入编码器的采样数累计。AAC 输出 AU 采用编码器返回的包 PTS，并结合编码器时间基换算；不要按收到包的序号重新生成 PTS。

实现要求：

- 使用 `int64_t` 表示内部 PTS，统一审核 `H264AccessUnit`、`AacAccessUnit`、`RtpPayloadUnit`、订阅起点及计算函数。
- 时间基换算使用 `av_rescale_q` 等安全整数运算；核心协议库可使用不依赖 FFmpeg 的安全换算辅助函数。
- 轨道比较不能直接比较 tick 数，也不能无检查地做多项乘法。使用经过溢出分析的有理数比较或共同时间单位。
- 微秒转换需要定义舍入规则，避免往返转换造成 PTS 不递增。
- 时间戳在采集/输入阶段确定，编码完成、入队、发送时不重新打媒体 PTS。
- 仅在写 RTP 头时按 32 位模空间生成时间戳；覆盖回绕测试，不把回绕误判成时间倒退。

### 4.4 设备时钟映射与精度边界

建议使用独立的设备时间映射器，职责与只读的共享时间起点分开：

```text
t_capture = device_offset + rescale(device_pts - device_pts0)
```

设备 PTS 优先使用，但不能默认摄像头和麦克风的 PTS 起点、单位或时钟域相同。需要确认 DirectShow 后端给出的时间戳语义，再建立与单调时钟的映射。

如果只能获得数据到达时刻，音频块到达时间通常晚于块内第一个采样。可用块时长反推并校准缓冲延迟，但这只是近似。保留设备 PTS、到达时刻、块长度和校准偏移用于排查；初版允许配置固定音频偏移，不能宣称仅凭首包到达时间即可达到精确同步。

设备时间戳缺失、回退或发生明显跳变时，要记录并执行明确策略：短暂缺失可在有界范围内外推；不连续变化先令会话失败收尾。第一版不把重新打开设备伪装成旧时间线的自然延续。

## 5. 新增模块与接口契约

| 建议文件 | 职责 |
| --- | --- |
| `src/media/audio_source.h` | 与 VideoSource 对称的音频源接口 |
| `src/media/file_audio_source.h/.cpp` | 包装 AacFileReader，保持现有文件行为 |
| `src/media/audio_frame.h` | 拥有 PCM 数据及设备 PTS、采样数、格式和共同时间信息 |
| `src/media/microphone_capture.h/.cpp` | DirectShow 麦克风枚举、打开、读取、停止和错误报告 |
| `src/media/audio_resampler.h/.cpp` | swresample 转换及受限漂移补偿 |
| `src/media/aac_encoder.h/.cpp` | PCM FIFO、AAC-LC 编码、时间戳和轨道配置 |
| `src/media/audio_frame_queue.h/.cpp` | 已编码音频 AU 的有界队列 |
| `src/media/microphone_audio_source.h/.cpp` | 采集编码线程、启动握手和 AudioSource 适配 |
| `src/media/capture_timeline.h/.cpp` | 共同起点、时间转换和共享参考 |
| `src/media/device_clock_mapper.h/.cpp` | 每个设备的时间映射及不连续检测 |
| `src/app/microphone_probe_main.cpp` | 不经过 RTSP 的独立设备与编码验证入口 |

模块可以在实现时合并小文件，但需保留以上职责边界。AudioSource 建议契约：

```cpp
class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual bool read(AacAccessUnit& frame) = 0;
    virtual bool ended() const = 0;
    virtual const MediaTrack& track() const = 0;
    virtual bool is_live() const { return false; }
    virtual void resume() {}
    virtual void stop() {}
};
```

`read()` 不等待：返回 false 且未结束表示 Waiting；关闭并排空后，消费者才锁定 ended 状态；不可恢复错误通过异常传递。复用现有 VideoSource 的状态约定，避免生产者结束标志与最后一帧入队之间的竞态。

新增接收 `unique_ptr<VideoSource>` 与 `unique_ptr<AudioSource>` 的会话构造方式。保留当前文件路径构造函数，由它创建对应文件源，避免破坏已有调用方式。

## 6. 麦克风采集与 AAC 编码

### 6.1 采集后端

沿用 FFmpeg `libavdevice` 的 DirectShow 输入，通过 `audio=设备名` 打开麦克风。[2]

- 枚举音频输入设备，提供编号和稳定可辨识的名称；启动参数支持名称或编号。
- 设备打开后读取实际采样率、声道和采样格式，不把请求参数当成已协商结果。
- 读取 PCM；后端输出需解码的数据时先解码为 AVFrame，再进入统一转换链路。
- 读取操作必须可停止，并在真实设备上验证停止时限；不能仅因为设置了 FFmpeg 中断回调就假定所有阻塞都可被打断。
- 调整 `audio_buffer_size` 时记录请求值和观察到的块时长。该值过大增加延迟，过小可能影响稳定性。[2]

### 6.2 格式转换与编码

```text
设备实际 PCM
    → swresample：转换到 48 kHz / 单声道 / 编码器支持的采样格式
    → AVAudioFifo：积累到编码器 frame_size
    → AAC-LC 编码
    → AacAccessUnit（裸 AAC + 包 PTS + duration）
    → AacPacketizer
```

- 第一版要求所选 AAC-LC 编码器每帧 1024 个采样，与现有 AAC 接口保持一致；启动时检查 `frame_size`，不满足时给出明确错误。
- 48 kHz 下每 AU 为 1024 / 48000 秒，约 21.33 ms；禁止按整数 21 ms 循环累加。
- 从编码器实际配置获得 AudioSpecificConfig 和声道信息，生成现有 MPEG4-GENERIC / AAC-hbr SDP；从编码参数生成配置，不通过临时 ADTS 文件反推。
- 持续排空编码器输出，正确处理 EAGAIN、多包输出和正常停止时 flush。实时错误时优先尽快失败收尾，不无限冲刷旧缓存。
- 单个 AAC AU 超过当前 packetizer 的大小上限时明确失败；第一版不增加 AU 分片。

### 6.3 编码延迟和起始填充

AAC 编码器可能包含 initial padding，首包 PTS 可能早于首个有效输入采样。[3] 内部保留有符号 PTS，检查编码器包时间与 padding；不要把负值直接变成巨大的无符号时间，也不要逐轨把负时间钳成 0。

需要完整记录“输入 PCM PTS → 编码包 PTS → RTP 时间 → 解码后脉冲位置”。实际接收端对裸 AAC 起始填充的处理存在兼容性边界，必须用播放器测试。若需要统一平移时间线，应对两轨与 RTCP 映射使用同一个偏移，并明确它不会自动消除解码器起始填充。

## 7. 调度、缓存与异常策略

### 7.1 实时调度与文件调度分开

现有文件模式可继续建立虚拟播放起点。实时模式的共同起点 T0 固定，不因首个客户端加入、队列暂时为空或恢复观看而重置。建议显式引入两种模式，避免复用 `reset_clock_` 时改变设备时间语义。

```text
实时帧的媒体参考时刻 = T0 + PTS / clock_rate
实时帧的计划发送时刻 = 媒体参考时刻 + send_buffer
```

`send_buffer` 两轨一致，可先用 150 ms 作为实验默认值。该值只是一项本机调度预算，不能保证总播放延迟；采集、编码和接收端缓冲都需要实测。

每次 tick：非阻塞预读两轨 → 比较媒体时间 → 按期限投递 → 更新下次期限并返回。每轮处理量保持有界，避免积压时占满网络事件循环。两轨的 Waiting 重查要对称：无论缺视频还是缺音频，都应取待发期限和短期重查期限中较早者；初版可延用 5 ms 重查。

### 7.2 队列规则

| 情况 | 建议处理 |
| --- | --- |
| 音频队列正常 | 依序输出，采样时间连续 |
| 少量音频缺口 | 在 PCM 阶段按缺失采样数补静音，限制单次与累计补偿量 |
| 音频生产者长期无数据 | 监测失败并收尾，不能依靠永远补静音掩盖故障 |
| 音频队列溢出或落后严重 | 丢弃整 AU 以回到时延预算，保留原 PTS 跳变并记录；不能重新编号掩盖丢失 |
| 视频编码前积压 | 丢弃过期原始帧，保持输入采集时间 |
| 已编码视频积压 | 清理依赖链后等待 IDR，不能丢 P 帧后继续发依赖它的帧 |
| 单个客户端出站队列满 | 沿用当前策略终止该慢客户端，其他客户端继续 |

音频队列按媒体时长与字节数双重限制，可先设置 200 ms 时长预算，并为 1024 采样的 AU 边界明确舍入规则。视频沿用现有帧数/字节数限制，再补充最旧帧年龄统计。容量参数需要根据采集、编码及压力测试调整。

补静音属于采样数据修复，丢过期编码 AU 属于实时延迟控制，两者不能混用：如果为已经过期的整段数据再逐帧补发静音，仍然无法追上实时进度。

### 7.3 晚加入与无人观看后恢复

- 新的双轨订阅以可解码 IDR 的媒体时间作为共同起点；音频选择起始时间不早于该起点的 AU，保留真实差值。边界量化误差最多约一个 AU，解码器预热误差另外测量。
- 纯音频订阅从当前合适 AU 开始，无需等待视频 IDR。
- 仅一个新客户端加入时，只为该客户端选择起点，不能清空其他观看者仍在使用的共享源。
- 所有客户端离开后生产者继续采集，有界丢弃缓存；首个观看者恢复时清理过期预读帧，选择满足实时年龄预算的 IDR。没有合适 IDR 则等待下一帧 IDR，不能用很旧的 IDR 快速补发历史内容。
- 若 IDR 等待影响体验，后续增加请求编码器产生 IDR 的接口；第一版先用 GOP 控制并测量最坏等待时间。
- 第一版双实时源默认采用严格故障策略：任一必需设备不可恢复失败，整个媒体会话失败收尾；文件模式原有的单轨 EOF 行为保持兼容。

## 8. RTCP 与 RTP 时间映射

保留现有 SR + SDES 机制。各轨有独立 SSRC、序号和采样频率，相关音视频轨道使用相同的稳定 CNAME。建议 CNAME 按本资源实例生成，避免所有独立源都使用相同硬编码值。[1]

对共同媒体时间 `t`，映射关系为：

```text
NTP = MediaClock.ntp_at(T0 + t)
video_rtp = video_rtp_base + rescale(t - subscription_start, 1/90000)
audio_rtp = audio_rtp_base + rescale(t - subscription_start, 1/Fs)
```

两条轨道 RTP 数值不需要相同。生成 SR 时，在同一个报告时刻计算 NTP 与该轨对应的 RTP 时钟值；若两条 SR 实际发送时刻不同，也各自保持一致映射，无需强制相同 NTP 数值。RTP base 可以不同，但必须纳入 SR 和订阅时间换算。

`MediaSubscription::deliver()` 把首次 RTP 时间锚定到 `media_at`。共同实时模式中 `send_buffer` 只影响计划发送时刻，不进入 RTP/RTCP 的采集时间映射；文件模式的媒体时刻仍使用虚拟播放起点。

SR 的包数/字节数沿用实际发送完成后的统计，不能使用编码数或入队数。为首个可用报告安排及时发送，后续沿用可配置周期，不依靠大幅提高 SR 频率修复错误 PTS。

## 9. 长时间运行的漂移补偿

共享起点只能解决起始偏移，设备采样速度仍可能与单调时钟略有不同。第一版先加入可观测性，再开启受限自动补偿。

```text
audio_end_us = 首段音频起点 + 已输出采样数换算的时长
reference_end_us = 根据设备时间戳和时钟映射得到的对应采样结束时刻
drift_us = audio_end_us - reference_end_us
```

对时间匹配的采样区间计算误差，不能直接把队列长度或“当前 now 减音频 PTS”当作设备漂移，因为其中还包含缓冲和线程调度。

- 过滤单次到达抖动，以数秒观察窗口估计持续趋势。
- 使用 `swr_set_compensation()` 在后续一段输入上小幅调整重采样输出数量。[4]
- 限制补偿速度和累计量，包含死区和方向测试，避免控制振荡。阈值作为可调实验参数，不能把某个固定毫秒数当成通用标准。
- 音频 PTS 继续按实际输出采样数累计；不周期性跳改 PTS，不反复重建共同起点。
- 视频设备时间也持续接受映射质量检查；时间基改变或明显不连续按故障处理。
- 固定同步偏移和时钟漂移分开记录：前者用于校准设备延迟，后者才由采样率补偿处理。

## 10. 构建、命令行与生命周期

已增加 `ENABLE_MICROPHONE_CAPTURE` 和 `ENABLE_AAC_ENCODING` 两个可选构建开关，沿用已有 FFmpeg 查找与 DLL 复制逻辑。采集依赖 `avdevice/avformat/avcodec/avutil`，编码与格式转换依赖 `avcodec/avutil/swresample`；启用相关功能时才引入依赖，默认核心协议构建仍不要求 FFmpeg。

启动时检查 AAC 编码器、采样格式、frame_size 和音频设备。音频源的 track 配置必须在生成 SDP 前就绪，配置发布后保持不变。任一源启动失败或用户取消时，回收已启动的另一个源，不遗留线程和设备句柄。

服务器输入和同步选项：

| 选项 | 含义 |
| --- | --- |
| `--list-microphones` | 已实现：只枚举麦克风，不采集 |
| `--microphone INDEX` | 已实现：选择麦克风，可单独使用或与摄像头组合 |
| `--microphone-device NAME` | 已实现：按名称选择麦克风，与 INDEX 互斥 |
| `--synthetic-audio` / `--synthetic-video` | 已实现：对应轨道使用合成输入，无需设备 |
| `--audio-bitrate N` | 待实现；当前 AAC 默认 64000 bit/s |
| `--send-buffer-ms N` | 已实现：双轨共同发送缓冲，0～200 ms，默认 150 ms |
| `--audio-offset-ms N` | 待实现：经校准的音频时间偏移；正值表示相同音频采样在媒体时间线上向后移动 |

第一版编码采样率和声道固定为 48000/1。使用真实设备时开启两个采集开关和两个编码开关：

```powershell
cmake -S . -B build-av -DENABLE_CAMERA_CAPTURE=ON -DENABLE_H264_ENCODING=ON -DENABLE_MICROPHONE_CAPTURE=ON -DENABLE_AAC_ENCODING=ON -DFFMPEG_ROOT="C:/path/to/ffmpeg-sdk"
cmake --build build-av --config Debug
.\build-av\Debug\rtsp_server.exe --list-microphones
.\build-av\Debug\rtsp_server.exe --camera 0 --microphone 0 --send-buffer-ms 150 --bind 127.0.0.1 --port 8554
```

## 11. 分阶段实施与完成条件

| 阶段 | 主要工作 | 完成条件 |
| --- | --- | --- |
| 1：抽象音频源 | 新增 AudioSource/FileAudioSource；保留构造兼容 | 现有文件双轨、纯音频和摄像头测试通过 |
| 2：独立采集 | 新增麦克风枚举、PCM 采集、probe 和可停止读取 | 有设备时连续采集正常；无设备时明确报错；停止能释放设备 |
| 3：独立音频编码（已实现） | 重采样、FIFO、AAC、配置生成、音频源、最小有符号 PTS；纯音频测试会话 | 已用外部 FFmpeg 验证解码、连续采样/PTS、padding 和脉冲位置；真实设备验收边界见实现说明 |
| 4：共同时间线（已实现） | 注入 CaptureTimeline；改造视频模式；分开采集时刻与发送期限 | 合成轨道验证 RTP/RTCP 保留 120 ms 启动差，并可反推原始采集时刻 |
| 5：实时行为（已实现） | 接通双轨服务器入口；处理晚加入、积压、无人观看和设备失败 | 合成输入通过双客户端、慢连接隔离、空闲恢复及故障回收测试 |
| 6：漂移与验收 | 统计、时钟映射验证、受限重采样补偿和长时间测试 | 模拟漂移可被控制，真实设备测试无持续偏差增长趋势 |

### 阶段 4 共同时间线实现

- 在启动设备前创建一个 `std::shared_ptr<const CaptureTimeline>`；将同一实例填到 `CameraVideoSourceConfig::timeline`、`MicrophoneAudioSourceConfig::timeline`，两路源就绪后传给 `MediaSession(video, audio, timeline)`。不注入则保留旧路径。
- 视频用首帧的单调到达时刻估计起始偏移，之后保留设备 PTS 增量。若首帧没有设备 PTS，则本轮用到达时刻；若从设备 PTS 模式中途丢失 PTS 或时间基改变，则报错。音频用首块到达时刻减块时长估计首采样，只整体平移 AAC 包 PTS，包含 flush 包和起始填充。到达时间只是估计，未校准实际设备延迟。
- 会话默认用 `T0 + PTS + 150 ms` 决定发送期限，用 `T0 + PTS` 建立 RTCP 锚点。订阅恢复不改变 `T0`，晚加入仍从可解码的 IDR 起步。
- `camera_video_source_tests` 和 `microphone_audio_source_tests` 用生成帧跑真实编码器，检查 80/200 ms 起点及负 AAC PTS。`media_session_tests` 用整数 tick 控制推进，验证发送期限、RTP 回绕、从不同报告时刻的 SR 反推出 80/200 ms，以及重新订阅不重置 `T0`。

### 阶段 5 双轨实时行为实现

- 双轨共用生产者和固定 `T0`。过了计划发送时刻 50 ms 的 AU 不再发送，丢 AAC 保留原 PTS，丢视频后等未过期的 IDR。默认设置下，约 200 ms 是共享调度的媒体年龄上限，不是客户端播放延迟。
- 新观众只等待自己的首个 IDR，不动共享队列。无人观看时继续有界采集；恢复时清除两轨预读、重新选 IDR，并按同一规则过滤缓存。双轨音频保留至多 9 个 1024-sample AU（48 kHz 下约 192 ms）和原有字节上限。
- 任一路生产失败、意外结束或持续无数据时，回收两路并结束直播订阅；空闲时也检查。慢连接仍由自身的出站队列或发送超时退出。
- 无设备测试：默认构建 17/17，全功能构建 40/40；实际启动合成双轨服务器后，两个本地 FFmpeg 客户端均解码了 H.264 和 AAC。尚未验证真实设备拔出和实机同步误差。

## 12. 测试与验收

### 12.1 不依赖实体设备的自动化测试

- 时间换算：不同时间基、舍入边界、负 PTS、长时间运算和 RTP 32 位回绕。
- 起始偏移：音频领先/落后 120 ms、两源不同启动顺序，经过编码和订阅换算后偏移仍正确。
- 编码链路：输入合成 PCM 脉冲，验证 1024 采样边界、FIFO 余量、重采样输出数、AAC 包 PTS、flush 和解码后的脉冲位置。
- 调度：两轨 Waiting 的对称唤醒、单轨短缺不阻塞网络事件、未来帧不过早发送、过期帧不无限补发。
- RTCP：对 SR 反推共同时间，检查 NTP/RTP 同时刻关系、不同采样率及发送缓冲不会产生相对偏移。
- 队列/订阅：音频容量与时间跳变、视频 IDR 恢复、晚加入、纯音频、多客户端和无人观看后恢复。
- 漂移：模拟正负 100 ppm、到达抖动与固定偏移，验证补偿方向、限速、不振荡及 PTS 不回退。
- 生命周期：启动超时、一个源失败后的另一源回收、生产者结束竞态、重复 stop 和异常传递。

以上测试应使用虚拟时钟或可控时间输入，避免把分钟级真实等待放进普通单元测试。保留默认构建回归，并为启用音频/视频的构建增加集成测试。

### 12.2 实体设备验收

1. 使用 FFmpeg 枚举麦克风并短录，先确认设备、权限和实际格式。
2. 通过 microphone_probe 连续采集，观察块长度、时间戳、编码结果和停止时限。
3. 启动双轨 RTSP，使用 ffplay/VLC 观察拍手画面与声音，重复多次并记录偏差。拍手测试用于人工观察，不能代替自动时间映射验证。
4. 连续运行 30～60 分钟，在开始、中途和结束重复测量，记录偏差是否随时间增长。
5. 验证第二个客户端晚加入、所有客户端退出后恢复、慢客户端以及设备断开。

建议把“稳定后可观测音画偏差不超过约 50 ms，60 分钟内无持续增长趋势”作为初始工程目标。它不是协议保证或已测结果；最终阈值须依据设备、播放器和测量方法确定。总播放延迟与音画相对偏差分别记录。

### 12.3 当前可用的外部工具命令

以下命令属于 FFmpeg/ffplay，不代表本仓库已经支持麦克风。短录命令会录制麦克风并生成文件，仅由操作者按需执行。[2]

```powershell
# 枚举设备；dummy 用于触发列表输出，随后退出报错不代表枚举失败。
ffmpeg -list_devices true -f dshow -i dummy

# 使用列表中的准确设备名，录制 5 秒。
ffmpeg -f dshow -i "audio=这里填写麦克风设备名" -t 5 -ar 48000 -ac 1 mic-test.wav

# 实现双轨服务器后播放验证。
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

基础与功能构建分别验收；功能构建需要先实现本文列出的开关与目标：

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure

ctest --test-dir build-av -C Debug --output-on-failure
```

## 13. 必要日志与实施边界

每秒聚合一次即可，避免逐包打印拖慢采集：

- 输入实际采样率/声道/采样格式、视频格式、源时间基与选定设备。
- 设备 PTS、共同时间 PTS、采集到达延迟估计与编码耗时分布。
- 音视频队列最旧帧年龄、帧数/字节数、丢弃量与等待 IDR 状态。
- 补静音采样数、音频漂移估计、重采样补偿量与时间不连续次数。
- 每客户端发送队列长度、SR 时间映射及慢连接退出原因。

固定偏移、时间戳错误、设备漂移、编码排队、网络排队分别定位。发现音画不同步时，先证明共同时间与 SR 映射正确，再调整缓冲或补偿参数，避免用延迟参数掩盖根因。

阶段 4 的验收使用合成输入，未验证真实双设备的采集延迟、漂移或播放器同步效果。

## 14. 参考资料

- [1] [RFC 3550：RTP 时间戳、RTCP Sender Report 与 CNAME](https://www.rfc-editor.org/rfc/rfc3550.html)，重点参考 5.1、6.4.1、6.5.1 节。
- [2] [FFmpeg Devices：DirectShow 输入、音频设备及 audio_buffer_size](https://ffmpeg.org/ffmpeg-devices.html#dshow)。
- [3] [FFmpeg AVCodecContext：initial_padding 与输出时间戳](https://ffmpeg.org/doxygen/trunk/structAVCodecContext.html)。
- [4] [FFmpeg libswresample：格式转换与 swr_set_compensation](https://ffmpeg.org/doxygen/trunk/group__lswr.html)。
- 仓库已有说明：`tcp_aac_implementation.md`、`rtcp_runtime_implementation.md`、`camera_video_source_implementation.md`、`h264_encoder_implementation.md`。
