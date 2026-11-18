# 麦克风独立采集：阶段 2 实现说明

本阶段实现 Windows DirectShow 麦克风枚举、PCM 采集、可停止读取和独立 probe。采集结果只在内存中使用，probe 只打印格式、时间信息和计数，不保存音频文件。AAC 编码、重采样和 RTSP 接入在后续阶段完成。

## 1. 模块与阅读顺序

| 文件 | 职责 |
| --- | --- |
| src/media/audio_frame.h | PCM 数据引用、采样格式、原始设备 PTS 和交付时的单调时钟时间 |
| src/media/microphone_capture.h | 配置、实际格式、读取结果和采集接口 |
| src/media/microphone_capture.cpp | DirectShow 枚举、FFmpeg 设备与 PCM 解码、读取等待和资源释放 |
| src/app/microphone_probe_main.cpp | 参数解析、设备选择、单线程采集循环、控制台停止和统计 |
| cmake/MicrophoneCapture.cmake | 独立可选构建、FFmpeg 依赖、DLL 复制与 CTest 注册 |
| tests/microphone_capture_tests.cpp | 无硬件配置测试，以及显式启用的真实设备测试 |

数据路径：麦克风 → DirectShow/FFmpeg 数据包 → PCM 解码器 → AudioFrame → probe。

MicrophoneCapture 输出原始 PCM；AudioSource 接口要求编码后的 AAC AU。因此这一阶段不继承 AudioSource，后续由 MicrophoneAudioSource 组合采集、重采样和 AAC 编码。

## 2. 构建与运行

在项目根目录使用 PowerShell。FFMPEG_ROOT 指向同时包含 include、lib、bin 的 FFmpeg shared SDK；下面示例使用本项目现有验证 SDK。

~~~powershell
cmake -S . -B build-microphone -DENABLE_MICROPHONE_CAPTURE=ON -DBUILD_TESTING=ON -DFFMPEG_ROOT=E:/MarkDown/rtsp-cpp/.verify/stage3-sdk/ffmpeg-master-latest-win64-gpl-shared
cmake --build build-microphone --config Debug
ctest --test-dir build-microphone -C Debug --output-on-failure

# 只枚举，不打开麦克风。
.\build-microphone\Debug\microphone_probe.exe --list

# 默认格式，持续采集 60 秒，仅输出首帧和汇总。
.\build-microphone\Debug\microphone_probe.exe --device-index 0 --seconds 60 --quiet

# 请求 48 kHz、单声道、20 ms 缓冲。设备不支持时明确报错，不自动重采样。
.\build-microphone\Debug\microphone_probe.exe --device-index 0 --sample-rate 48000 --channels 1 --buffer-ms 20 --seconds 3 --repeat 3

# 持续运行，按 Ctrl+C / Ctrl+Break 请求停止。
.\build-microphone\Debug\microphone_probe.exe --device-index 0 --seconds 0

# 显式执行真实设备测试：输出缓冲持有、时间信息、停止和重开。
.\build-microphone\Debug\microphone_capture_tests.exe --device-index 0
~~~

默认关闭 ENABLE_MICROPHONE_CAPTURE。该选项可单独开启，也可与 ENABLE_CAMERA_CAPTURE、ENABLE_H264_ENCODING 同时开启。

probe 不传设备时选择当前枚举的第一只输入设备。--device 接受 UTF-8 友好名称或 --list 显示的 DirectShow 替代名称，与 --device-index 互斥。枚举顺序可能变化，需要固定设备时使用名称。Windows 使用宽字符入口处理中文参数。

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| --list | 关闭 | 列出设备；无设备时打印提示并正常退出 |
| --device NAME | 空 | 按名称选择 |
| --device-index N | 第一只 | 按枚举索引选择 |
| --sample-rate N | 0 | 请求采样率，0 使用设备默认 |
| --channels N | 0 | 请求声道数，0 使用设备默认 |
| --buffer-ms N | 0 | 请求设备缓冲时长，0 使用设备默认 |
| --seconds N | 10 | 每轮时长，0 持续运行 |
| --timeout-ms N | 5000 | 连续多久没有有效 PCM 后失败 |
| --repeat N | 1 | 每轮结束关闭设备，再重新打开 |
| --quiet | 关闭 | 只打印首帧和汇总；普通模式约每秒打印一次 |
| --help | 关闭 | 打印帮助 |

## 3. 接口和生命周期

MicrophoneConfig 只提供设备名称、采样率、声道数和缓冲请求。后三者为 0 时不向 DirectShow 传对应选项；实际格式通过 format() 查询。请求格式与实际协商结果不一致时直接失败。

MicrophoneCapture 使用 Impl 隐藏 FFmpeg、COM 与等待状态。调用方在同一个线程构造/打开、读取、查询和关闭；仅 request_stop() 可以由其他线程调用。采集类不创建自己的工作线程，也不增加 PCM 队列。

- open：先关闭旧资源，验证配置，安装外部停止标记，打开音频流并初始化 PCM 解码器。设备名必须明确提供。
- read：清空输出，先尝试取得解码输出，再读取并提交设备包。输出 PCM 仍保持设备实际格式与帧长度，不假定每帧 1024 个采样。
- request_stop：设置原子标记并唤醒本类条件变量，不在调用线程释放设备。
- close：由读取所属线程释放 AVFrame、AVPacket、AVCodecContext、AVFormatContext，清除实际格式；可重复调用。
- 析构：调用 close；调用者须先结束跨线程停止操作，再销毁对象。

外部 std::atomic<bool> 可在打开前传入，必须存活到 close。已经设置的外部取消不会被 open 重置。新一次 open 会清除本对象上一次运行的内部停止状态。

## 4. read 的结果与等待

| 结果 | 说明 |
| --- | --- |
| FrameReady | 输出一份有效 AudioFrame |
| Timeout | 本次等待截止时仍没有 PCM，可继续读取 |
| Stopped | 收到内部或外部停止请求，调用方退出循环并关闭 |
| Failed | 未打开、设备读取失败、解码失败或格式异常，通过 last_error 查询原因 |

参数错误和打开失败通过异常报告；运行中的设备读取错误通过 Failed 报告并锁定失败状态。除 FrameReady 外，输出均为空。负超时为参数错误；0 表示尝试一次非阻塞读取及其可用解码输出。

输入设置 AVFMT_FLAG_NONBLOCK，无包时处理 EAGAIN，在条件变量中每次最多等 5 ms，且不超过 read 的截止时间。request_stop 会立即唤醒；外部停止标记在短期检查时观察。probe 每次最多读取等待 50 ms，再检查运行时长与无数据期限。

中断回调同时观察停止标记，但不承诺所有驱动的设备初始化调用都有硬超时。read 的等待超时与 open 的耗时是两个概念。设备打开和关闭的实际耗时通过测试与日志观察。

## 5. PCM 所有权与时间信息

AudioFrame.buffer 使用引用计数持有 AVFrame。交付时建立独立的帧引用，read 内部的复用、unref 和设备关闭不会使已交付的 PCM 失效。完整声道布局在 buffer->ch_layout，PCM 平面通过 buffer->extended_data 访问。

nb_samples 为每个声道的采样数，不是字节数。累计 PCM 时长按 sum(nb_samples) / sample_rate 计算，不再额外乘声道数。

source_pts 使用有符号 64 位可选值，保持设备原始值；有效帧自带 time_base 时使用它，否则使用输入流时间基。缺失的 PTS 保持缺失，不伪造为 0。arrival_steady_us 是帧交付时刻，不是硬件采样时刻，不能直接拿来声明音视频已同步。

## 6. 测试与实测记录

2026-11-18 在本机“麦克风 (Realtek High Definition Audio)”上验证：

- 配置测试、非法设备、提前取消、重复关闭、probe 参数错误均通过。
- 显式硬件测试完成三轮打开、读取、跨线程停止、关闭、重开；保留的 PCM 引用在后续读取和关闭后仍有效；另验证外部停止标记。
- 三轮停止唤醒约 0.022～0.071 ms，close 约 9.5～11.5 ms。这是本机单次测试记录，不是跨设备保证。
- 默认格式为 pcm_s16le、44100 Hz、2 声道、s16，时间基 1/10000000；每包 22050 个采样，约 500 ms。
- 60 秒采集窗口取得 119 帧、2623950 个采样/声道，即 59.5 秒 PCM；总耗时约 60.528 秒，包含设备打开与关闭，close 约 10.807 ms。
- 默认批次较大，窗口结束时不等待下一包，因此采样时长与采集窗口并非严格相等，不能仅根据这组统计推断时钟漂移。
- 请求 48000 Hz、单声道、20 ms 缓冲后，实际每包 960 个采样，三轮 3 秒采集均可关闭重开。

回归结果：基础构建 16/16、独立麦克风构建 22/22、原摄像头与编码构建 25/25、摄像头与编码加麦克风组合构建 31/31，均通过。麦克风构建的默认 CTest 不自动选设备采集。真正没有设备时，--list 打印空列表提示，请求采集时报错退出；当前机器有设备，已验证不存在的设备名，物理无设备、设备拔出和更长时间耐久测试仍需相应环境验收。

后续阶段在保留本模块接口的基础上加入重采样、PCM FIFO 与 AAC 编码，再接入 AudioSource。
