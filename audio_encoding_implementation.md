# 独立音频编码：阶段 3 实现说明

日期：2026-11-18。输出固定为 AAC-LC、48000 Hz、单声道，默认码率 64000 bit/s。

## 1. 框架和阅读顺序

```text
音频工作线程
  MicrophoneCapture / 合成 PCM
    → AudioResampler
    → AVAudioFifo
    → AAC 编码器
    → AudioFrameQueue

网络事件循环
  MicrophoneAudioSource.read()
    → MediaSession
    → AacPacketizer
    → RTP/TCP
```

| 文件 | 职责 |
| --- | --- |
| `src/media/audio_resampler.h/.cpp` | 用一个 SwrContext 转成编码器的采样格式、48 kHz 单声道；排空延迟采样 |
| `src/media/aac_encoder.h/.cpp` | 管理重采样器、AVAudioFifo、AAC 编码、AudioSpecificConfig 和采样统计 |
| `src/media/audio_frame_queue.h/.cpp` | 用 deque 和一把互斥锁传递完整 AAC AU，限制数量及字节数 |
| `src/media/microphone_audio_source.h/.cpp` | 一个采集编码线程、首包握手、非阻塞读取、停止及异常传播 |
| `src/media/microphone_capture_input.h/.cpp` | 将已有 MicrophoneCapture 接到 AudioFrameInput；同线程打开和关闭设备 |
| `src/media/media_time.h` | 不依赖 FFmpeg 的有符号时间比较、时基换算 |
| `src/app/aac_encode_probe_main.cpp` | 独立编码入口，输出调试 ADTS、CSV 时间记录和 JSON 统计 |
| `src/app/synthetic_audio_input.h` | 不依赖设备的 333 采样分块输入，含音调和已知位置脉冲 |

建议先看 `AacEncoder::encode/flush`，再看 `MicrophoneAudioSource::produce`，最后看队列和输入适配器。

没有为重采样、FIFO 或编码另外创建线程。只有 AAC 队列跨线程；PCM FIFO 始终由音频工作线程访问。

## 2. 编码和时间约定

`AacEncoder::open()` 打开 FFmpeg 原生 `aac` 编码器，从实际支持列表选择输入格式，检查 1024 采样帧长，并直接将编码器 extradata 转成 SDP 的 AudioSpecificConfig。不通过 ADTS 文件反推配置。

`encode()` 按实际转换输出数写入 FIFO；只要凑够 1024 个采样，就取出并编码。每次提交后接收所有可用编码包。输入格式在流中发生变化时明确报错。

PCM 的时间基为 `1/48000`，输入编码器的 PTS 按成功提交的采样数累计：0、1024、2048……；AU 使用编码器输出包的原始 PTS 和 duration。采集帧的设备时间保持在 AudioFrame 中，阶段 3 不把不同设备映射到共同起点。

AAC 编码输出可能从负时间开始。`AacAccessUnit`、`H264AccessUnit`、`RtpPayloadUnit` 和订阅起点统一使用 `int64_t`。RTP 仍使用 `uint32_t`，只在订阅边界计算相对时间并叠加本轨道起点，正常支持回绕。

网络音频轨道用本轨道的非零 SSRC 值作为 RTP 起点，避免 FFmpeg 将零当作尚未建立的 base_timestamp，在收到首个 RTCP SR 后重新设定起点。内部直接构造的订阅仍可以显式选择零起点。跨轨比较应先换回媒体时间，不能直接比较不同 RTP 原点的数值。

## 3. 正常结束与主动停止

正常输入 EOF：

1. 排空重采样器，将实际剩余采样写入 FIFO。
2. 编码完整的 1024 采样块。
3. 最后一块不足 1024 时补静音，并记录 `tail_padding`。
4. 向编码器发送空帧，接收至 EOF。
5. 关闭 AAC 队列；消费者读完最后一个 AU 后确认 ended。

重复 `flush()` 返回空集合，flush 后继续 encode 会报错。`close()` 只释放资源。

主动 `stop()` 请求输入停止并唤醒等待，回收工作线程，丢弃待发 AU；不通过无限发送历史数据延长停止时间。不可恢复输入错误会在已有 AU 消费完后通过 read 抛出。

队列默认最多 32 个 AU、128 KiB。超过容量时丢弃最旧完整 AU并计数，保留后续原始 PTS。恢复订阅时保留最新 AU；连续性验收中检查 dropped 为零。更完整的音频缺口补偿、晚加入和无人观看策略留给后续阶段。

## 4. 构建和运行

在项目根目录执行，`FFMPEG_ROOT` 指向含 include、lib、bin 的 shared SDK：

```powershell
cmake -S . -B build-audio -DENABLE_AAC_ENCODING=ON -DENABLE_MICROPHONE_CAPTURE=ON -DFFMPEG_ROOT="C:/path/to/ffmpeg-sdk"
cmake --build build-audio --config Debug
ctest --test-dir build-audio -C Debug --output-on-failure
```

`ENABLE_AAC_ENCODING=ON` 可独立启用，关闭麦克风采集时仍支持合成输入、编码测试和合成音频 RTSP。两个选项默认均为 OFF；rtsp_core 不依赖 FFmpeg。

先用合成 PCM 验证编码，输出目录必须已存在：

```powershell
.\build-audio\Debug\aac_encode_probe.exe --synthetic --seconds 1 --output build-audio/probe.aac
ffmpeg -v error -y -i build-audio/probe.aac -f f32le build-audio/probe.f32
```

输出三个文件：`probe.aac`、`probe.aac.csv`、`probe.aac.json`。CSV 分别记录原始 PCM 的设备 PTS/采样率和 AAC 包 PTS/48000 Hz；JSON 记录输入数、实际重采样输出数、提交数、头尾填充及包数量。

合成输入特意多产生 17 个 44.1 kHz 采样，覆盖不完整尾帧，而不是只测试整数秒边界。

真实麦克风的独立编码和纯音频服务器：

```powershell
.\build-audio\Debug\rtsp_server.exe --list-microphones
.\build-audio\Debug\aac_encode_probe.exe --device-index 0 --seconds 5 --output build-audio/microphone.aac
.\build-audio\Debug\rtsp_server.exe --microphone 0 --port 8554
```

按名称选择设备时，probe 使用 `--device "设备名称"`，服务器使用 `--microphone-device "设备名称"`。参数从 Windows 宽字符转换为 UTF-8。

无需设备的服务器：

```powershell
.\build-audio\Debug\rtsp_server.exe --synthetic-audio --port 8554
```

在另一个终端播放：

```powershell
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

自带 rtsp_client 仍主要用于保存 H.264；纯音频解码使用外部 FFmpeg/ffplay。阶段 3 不接受摄像头和麦克风同时启动。

## 5. 自动化验收与实测证据

| 测试 | 覆盖 |
| --- | --- |
| `audio_frame_queue_tests` | 数量/字节限制、保留 PTS、结束/错误前排空、负时间与大时间值换算 |
| `aac_encoder_tests` | 0/1/1023/1024/1025/1500 采样、分块 FIFO、32/44.1/48/96 kHz、平面 PCM、配置、flush、解码数量和脉冲位置 |
| `microphone_audio_source_tests` | 等待、启动超时/取消、输入异常、PCM 饥饿、重复 stop、同线程关闭、纯音频会话、RTP 回绕和最终 RTCP |
| `audio_encoding_integration` | 外部 FFmpeg 解码 ADTS、核对脉冲和 padding，通过 RTSP/TCP 收取 48 个连续编码包并实际解码 |

外部集成测试在配置时发现 Python 和 ffmpeg 后自动注册，所有测试均无需打开实体麦克风或摄像头。

本机 FFmpeg 9.0.2 的外部验收结果位于 `build-audio/audio_integration/result.json`：

| 项目 | 结果 |
| --- | --- |
| 输入 | 44117 采样，44100 Hz，双声道 |
| 重采样后有效采样 | 48019 |
| 提交编码器 | 48128 |
| 手动尾部补零 | 109 |
| 编码器 initial_padding | 1024 |
| AAC 首包 PTS | -1024 |
| 外部解码输出 | 49152 采样 |
| 脉冲解码位置 / 预期位置 | 13024 / 13024 |
| RTSP 包数 / 相邻 PTS 增量 | 48 / 1024 |

采样账目：`49152 = 1024 + 48019 + 109`。

脉冲账目：输入位于 `11025 / 44100 = 0.25 秒`；转为 48 kHz 后位于第 12000 个有效采样，加上 1024 起始填充，解码位置为 13024。

纯音频会话测试另保存 `audio-session-rtp.csv` 和 `audio-session-wrap-rtp.csv`，记录负 AAC PTS 到零起点或回绕 RTP 时间的映射。

本次构建验证：默认功能 17/17、AAC 加麦克风采集 31/31、音视频功能全开 40/40 测试通过；仅启用 AAC、不启用麦克风采集时，三个关键编码/会话/外部测试通过。

## 6. 当前边界

- 本次自动验收使用合成 PCM，没有新录制真实麦克风；真实设备听感、连续运行和拔出需要实机验证。
- 裸 AAC/ADTS/RTP 不保证接收端自动裁掉起始和结束填充；这里保留原始包时间、统计填充并提供解码证据，没有宣称实现无缝裁剪。
- 共同 CaptureTimeline、摄像头启动偏移、双轨实时调度、补静音和设备漂移补偿属于阶段 4～6。
