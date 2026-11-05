# 摄像头采集阶段 2 实现说明

## 当前范围

阶段 2 提供独立的 Windows 原始视频采集链路：枚举设备、固定采集模式、取得原始帧、保留时间信息、停止和释放。原有 RTSP 文件播放接口保持原样。

本机已确认 `Integrated Camera` 支持 1280x720、30 fps、`yuyv422`。首版配置默认采用这组参数，也可选择设备支持的其他尺寸以及 `bgr24`。仅支持 rawvideo 输入；MJPEG 解码、像素转换、H.264 编码、实时队列和 RTSP 接入留在后续阶段。

## 构建

使用 Windows、CMake、Visual Studio 2022 C++ 工具链和 FFmpeg shared 开发包。开发包应包含 `include/libavdevice/avdevice.h`、`lib/*.lib` 和 `bin/*.dll`；开发库与 DLL 必须来自同一套包，位数与编译目标一致。

在项目根目录运行。本机 WinGet 安装的 FFmpeg 可这样定位：

```powershell
$ffmpegSdk = Split-Path -Parent (Split-Path -Parent (Get-Command ffmpeg).Source)
cmake -S . -B build-camera -DENABLE_CAMERA_CAPTURE=ON "-DFFMPEG_ROOT=$ffmpegSdk"
cmake --build build-camera --config Debug --parallel
ctest --test-dir build-camera -C Debug --output-on-failure
```

如果 `ffmpeg.exe` 来自 shim 或其他目录，手动把 `FFMPEG_ROOT` 设为真正包含 `include/lib/bin` 的 SDK 根目录。运行库不在该根目录的 `bin` 下时，可另传 `-DFFMPEG_RUNTIME_DIR=实际DLL目录`。

摄像头功能默认关闭，原构建命令不需要 FFmpeg。开启后增加 `camera_capture`、`camera_probe` 和 `camera_capture_tests`，不向 `rtsp_core` 链接 FFmpeg。CMake 在构建可执行文件后复制 SDK 的运行时 DLL，复制步骤也包含 FFmpeg DLL 的同目录依赖。

## 运行

```powershell
# 枚举视频设备，打印本次索引、显示名称和 alternative name。
.\build-camera\Debug\camera_probe.exe --list

# 默认参数：Integrated Camera、1280x720、30 fps、yuyv422，只读一帧。
.\build-camera\Debug\camera_probe.exe

# 按枚举索引选择设备，连续采集 10 秒。
.\build-camera\Debug\camera_probe.exe --device-index 0 --seconds 10 --quiet

# 显式指定采集模式。
.\build-camera\Debug\camera_probe.exe --device 'Integrated Camera' --width 1280 --height 720 --fps 30 --pixel-format yuyv422 --frames 1

# 验证关闭并重新打开，每次采集一帧。
.\build-camera\Debug\camera_probe.exe --frames 1 --repeat 3

# 持续采集，按 Ctrl+C 或 Ctrl+Break 正常停止。
.\build-camera\Debug\camera_probe.exe --frames 0 --quiet

# 查看全部参数。
.\build-camera\Debug\camera_probe.exe --help
```

`--device` 接受显示名称或 alternative name；`--device-index` 是本次枚举结果的下标，设备增减后可能改变，不能作为永久标识保存。两种选项互斥。使用同名设备时建议 alternative name 或索引。

默认一帧；`--seconds N` 在未显式指定 `--frames` 时改为按时长采集。两者同时指定时任一条件满足即停止。`--frames 0` 表示无帧数上限。`--repeat N` 每轮关闭、重新打开，时长和帧数限制按轮计算。`--quiet` 只保留每轮首帧日志和汇总。默认无帧超时为 5000 ms，可用 `--timeout-ms` 调整。

## 模块与调用顺序

```text
camera_probe_main.cpp
    ├─ CameraCapture::enumerate_devices()
    ├─ CameraCapture::open(config)
    ├─ 循环 CameraCapture::read(frame, timeout)
    │      └─ rawvideo AVPacket → 原始 AVFrame → CameraFrame
    ├─ CameraCapture::request_stop()
    └─ CameraCapture::close()
```

| 文件 | 职责 |
| --- | --- |
| `src/media/camera_capture.h/.cpp` | 设备枚举、模式核对、采集、有限等待、停止、清理 |
| `src/media/camera_frame.h` | 拥有帧引用，并携带格式与时间信息 |
| `src/app/camera_probe_main.cpp` | 参数解析、控制台停止、无帧超时和日志统计 |
| `cmake/CameraCapture.cmake` | 可选 SDK 查找、独立目标和运行库复制 |
| `tests/camera_capture_tests.cpp` | 无硬件契约检查和可显式运行的硬件检查 |

枚举通过原生 DirectShow 的 `ICreateDevEnum` 完成，返回显示名称和供 FFmpeg 打开的 alternative name，避免依赖不同 FFmpeg 版本对设备枚举接口的支持差异。设备读取使用 `dshow` 输入。

`open()` 明确传入尺寸、帧率和像素格式，并核对实际流参数。设备返回的帧率可能是 `10000000/333333`，与 30 fps 有量化误差，核对允许 0.1% 的相对误差。尺寸和像素格式必须一致。首版 rawvideo 参数在打开时可用，因此不调用可能额外读取数据的 `avformat_find_stream_info()`。

同一轮的 `open()`、`read()`、`close()` 和析构必须由同一所有者线程顺序调用；仅 `request_stop()` 可由其他线程调用。不得在读取尚未退出时并发 `close()`。后续阶段接入采集线程时，先请求停止、等待采集循环返回并清理，再 join。

`read()` 的结果为：

| 结果 | 含义 | 调用者动作 |
| --- | --- | --- |
| `FrameReady` | 获得一份有效原始帧 | 使用该帧 |
| `Timeout` | 本次等待期内没有帧 | 可重试，或按累计无帧时间结束 |
| `Stopped` | 已收到停止请求 | 结束循环并清理 |
| `Failed` | 设备读取或解码发生不可恢复错误 | 读取 `last_error()`，清理后决定是否重开 |

读取开始时清空输出帧，未取到帧时不会返回上次的残留数据。打开失败会清理已创建资源并抛出异常；`close()` 可以重复调用，析构也会清理。`open()` 会先关闭上次输入，再开始新一轮。

## 帧内存和时间

`CameraFrame::buffer` 持有引用计数的只读 `AVFrame`。后续读取和关闭摄像头不会使已返回的帧失效；最后一份引用释放时回收内存。消费者应及时释放旧帧，连续保留会占用内存。像素数据按 `AVFrame::data`、`linesize` 读取；使用者需要访问这些成员时显式包含 FFmpeg 的 `libavutil/frame.h`。

| 日志字段 | 含义 |
| --- | --- |
| `n` | 本轮帧序号，从 0 开始 |
| `size / fmt / linesize` | 实际原始帧尺寸、像素格式、各平面行跨度 |
| `source_pts / time_base` | 源时间戳与单位，不做从零改写 |
| `source_duration` | 源帧时长，单位与源时间基相同，0 表示未提供 |
| `relative_pts_us` | 有源时间戳时，相对本轮首个有效时间戳的微秒数 |
| `arrival_steady_us` | 本地取得该帧时的单调时钟值，非日历时间 |

时间戳缺失时输出 `missing`，不伪造源 PTS。摄像头提供的 `1/10000000` 时间基意味着一个 tick 为 100 ns；`duration=333333` 约为 33.3333 ms。这里不转换到 RTP 90 kHz。

汇总中的 `received_fps` 为本轮取得的帧数除以采集循环耗时，包含首帧等待、不含设备打开/关闭；`source_fps` 则用首尾有效 PTS 估算。短时间采集会受到启动缓存和抖动影响，不能用单帧统计判断稳定帧率。

## 停止、异常和验收

DirectShow 读取开启 `AVFMT_FLAG_NONBLOCK`。无数据时取得 `EAGAIN`，最多短等 5 ms 后重试；`request_stop()` 设置原子标记并唤醒等待。`read()` 的 deadline 约束读取循环，不能保证第三方驱动内部打开和关闭调用的耗时。

控制台处理函数只设置退出标记，主循环每次最多等待 100 ms，再检查标记和累计无帧时间。显式停止返回成功；读取失败或无帧超时返回非零状态。无帧超时报告采集停滞，不直接断言设备已拔出。

自动 CTest 不打开摄像头，覆盖配置拒绝、关闭状态读取、错误输出清空、停止状态、重复关闭和命令行帮助/错误。显式实机检查：

```powershell
.\build-camera\Debug\camera_capture_tests.exe --device 'Integrated Camera'
```

该检查覆盖打开失败后恢复、正确尺寸/格式/时间戳、跨读取和关闭的帧引用、非阻塞无帧返回和输出清空、跨线程停止及三轮重新打开。

本机已验证设备枚举、三次单帧打开、10 秒连续采集（302 帧）、错误设备和不支持的 25 fps 请求；16/16 CTest 通过。另向持续采集进程发送 Ctrl+Break，程序打印 `reason=console_stop`、约 0.45 秒完成停止和关闭、退出码为 0，随后再次单帧打开成功。关闭摄像头构建选项的独立目录也成功构建客户端和服务器。物理拔出未验证，需用可拔出的 USB 摄像头按以下步骤验收：

1. `--list` 确认外接设备索引，确认该设备支持的采集模式。
2. 以匹配参数运行 `--frames 0 --quiet --timeout-ms 3000`。
3. 拔出 USB，确认打印读取失败或无帧超时，程序释放并以非零状态退出。
4. 插回后再次枚举，重新运行单帧命令，确认能打开和释放。
5. 正常持续采集时按 Ctrl+C，确认打印 `reason=console_stop`，随后能重新打开。

