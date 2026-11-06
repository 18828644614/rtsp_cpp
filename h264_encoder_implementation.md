# 摄像头阶段 3：H.264 编码实现说明

## 范围与数据流

本阶段把 `CameraFrame` 转成完整的 `H264AccessUnit`，并提供 SPS/PPS、90 kHz PTS/duration 和 IDR 标志。采集线程、有界队列、`CameraVideoSource`、`MediaSession` 实时调度与 RTSP 接入仍在后续阶段。

```text
CameraCapture 或探测程序生成帧
    → CameraFrame（YUYV422 / BGR24，只读、可带行填充）
    → H264Encoder 内部 libswscale 转换成 YUV420P
    → libavcodec/libx264（无 B 帧、低延迟）
    → Annex-B 包拆为不含起始码的 NALU
    → H264AccessUnit + SPS/PPS 缓存
    → 独立 .h264 文件和 .h264.csv 时间记录
```

编码使用 FFmpeg 库，不启动 FFmpeg 子进程。独立 `ffmpeg`/`ffprobe` 命令仅用于外部验收。

## 构建

默认两个选项均为 OFF，原客户端、服务器和测试不链接 FFmpeg。编码可以独立开启，不需要打开摄像头，也不依赖摄像头采集目标。

SDK 必须包含同一套 `include`、导入库 `lib`、运行库 `bin`，提供 `avcodec`、`avutil`、`swscale`，且 FFmpeg 编译时启用了 `libx264`。使用 shared 开发包，仅有 `ffmpeg.exe` 不够。采集另需 `avdevice`、`avformat` 和 Windows DirectShow。位数与目标一致。

```powershell
# 只启用编码。把该路径替换为实际 SDK 根目录。
$ffmpegSdk = 'C:/tools/ffmpeg-shared'
cmake -S . -B build-encoder -DENABLE_H264_ENCODING=ON "-DFFMPEG_ROOT=$ffmpegSdk"
cmake --build build-encoder --config Debug --parallel
ctest --test-dir build-encoder -C Debug --output-on-failure

# 同时启用采集和编码，才能使用真实摄像头输入。
cmake -S . -B build-camera-encoder -DENABLE_CAMERA_CAPTURE=ON -DENABLE_H264_ENCODING=ON "-DFFMPEG_ROOT=$ffmpegSdk"
cmake --build build-camera-encoder --config Debug --parallel
ctest --test-dir build-camera-encoder -C Debug --output-on-failure
```

`cmake/FFmpeg.cmake` 共用 SDK 查找和 DLL 复制；`CameraCapture.cmake` 与 `H264Encoding.cmake` 分别声明自己的目标与组件依赖。SDK 的 DLL 会复制到相应可执行文件目录。运行库另置时使用 `-DFFMPEG_RUNTIME_DIR=实际DLL目录`。

MSVC 示例的程序位于 `Debug/`；MinGW 单配置构建位于构建目录根目录。

## 探测程序

```powershell
# 无摄像头：生成 300 帧，验证 1280x720、30 fps 的编码链路。
.\build-encoder\Debug\h264_encode_probe.exe --synthetic --frames 300 --output build-encoder/sample.h264 --quiet

# BGR24 输入、25 fps；尺寸必须为偶数。
.\build-encoder\Debug\h264_encode_probe.exe --synthetic --pixel-format bgr24 --width 640 --height 360 --fps 25 --gop 25 --frames 100 --output build-encoder/bgr.h264 --quiet

# 分数帧率 30000/1001。
.\build-encoder\Debug\h264_encode_probe.exe --synthetic --fps 30000 --fps-den 1001 --frames 100 --output build-encoder/fractional.h264 --quiet

# 有摄像头时：先枚举，再选择设备及其实际支持的模式。
.\build-camera-encoder\Debug\h264_encode_probe.exe --list
.\build-camera-encoder\Debug\h264_encode_probe.exe --device-index 0 --width 1280 --height 720 --fps 30 --frames 300 --output build-camera-encoder/camera.h264 --quiet

# 持续采集，Ctrl+C / Ctrl+Break 结束；正常停止后排空编码器并关闭文件。
.\build-camera-encoder\Debug\h264_encode_probe.exe --device-index 0 --frames 0 --output build-camera-encoder/continuous.h264 --quiet

# 外部验收：检查解码无错误，并核对实际帧数、格式和 B 帧设置。
ffmpeg -v error -i build-encoder/sample.h264 -f null -
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=codec_name,width,height,pix_fmt,has_b_frames,nb_read_frames -of json build-encoder/sample.h264
ffplay -framerate 30 build-encoder/sample.h264
```

默认输入为生成帧，默认 60 帧；生成速度不按墙上时钟限速。`--output` 必填，会覆盖该文件及同名加 `.csv` 的记录文件。`--frames 0` 持续处理直到控制台停止。默认码率 2000000 bit/s，未显式设置 GOP 时采用约一秒的帧数。完整参数见 `--help`。生成帧与设备选择互斥；仅启用编码时选择摄像头会给出明确错误。

日志包含每个 AU 的 PTS、duration、IDR、NALU 数量和字节数；`--quiet` 保留首个 AU 和汇总。CSV 列为 `index,pts_90k,duration_90k,idr,nalus,bytes`。汇总打印实际 SPS/PPS 生成的 fmtp、时间模式和时间戳回退帧数。裸 `.h264` 不保存逐帧 PTS/duration，精确时间核对须使用 CSV 或内存接口；重新读取该文件的现有文件源仍按固定 25 fps 生成时间。

## 接口与阅读顺序

| 文件 | 职责 |
| --- | --- |
| `src/media/h264_encoder.h` | 编码配置、送帧/排空接口、参数集与时间模式 |
| `src/media/h264_encoder.cpp` | 像素转换、编码、AU 解析、时间映射和 RAII 清理 |
| `src/media/h264_access_unit.h` | 共用输出格式：完整 NALU 集合、IDR、90 kHz 时间 |
| `src/app/synthetic_camera_frame.h` | 探测程序生成带时间戳的原始帧 |
| `src/app/h264_encode_probe_main.cpp` | 输入选择、控制台停止、编码、文件/CSV 输出 |
| `tests/h264_encoder_tests.cpp` | 编码后实际解码、质量/时间/边界/文件兼容性检查 |

`open(config)` 固定本轮尺寸、输入格式、帧率、码率和 GOP；支持正的偶数尺寸、1～240 fps 和 YUYV422/BGR24。使用 `libx264`、`veryfast`、`zerolatency`、Constrained Baseline、YUV420P、两个 slice 线程，关闭 B 帧、场景切换关键帧和 open GOP。输出带 AUD，IDR 重复 SPS/PPS。码率为 ABR 目标，不保证每一秒字节数完全相同。

首版颜色约定固定为 BT.601：YUYV 输入按 limited range、BGR 输入按 full range，转换后输出 limited-range YUV420P，并在 H.264 VUI 中标记 BT.601/SMPTE170M。未标记的 YUYV 按此约定处理；明确标记为其他矩阵或 full range 的 YUYV 会拒绝，不能静默忽略元数据。其他颜色配置留待扩展。

`encode(frame)` 不修改调用者的 AVFrame，使用实际 `data/linesize` 转换像素。输出为零到多个 AU，调用者必须处理整个集合。`flush()` 发送结束信号并读取至编码 EOF，核对已提交图像全部返回；重复排空返回空集合。排空后再送帧会报错，需要重新 `open()`。

`close()` 只释放资源，不隐式排空；析构也清理。输入/配置错误在送帧前拒绝，调用者可以更正输入继续使用；编码处理失败则必须重新打开。重新打开先关闭原编码器。所有操作由单个所有者线程顺序调用。

## AU、参数集与内存

本阶段限定 libx264 的“一包一个图像 AU”输出契约，并核对每包以唯一 AUD 开始、包含图像 NALU。多 slice 图像保持在同一 AU。它不是对任意第三方 H.264 字节流的通用 AU 分割器。

NALU 类型 7/8 更新 SPS/PPS 缓存，类型 5 设置 `is_keyframe`。没有有效参数集时拒绝输出图像；若 IDR 缺少参数集则使用缓存补齐。首次编码输出前 `sps()`/`pps()` 可以为空，不能在编码器打开后立即假定 SDP 已就绪。

NALU 保存头和负载，不含 Annex-B 起始码；文件写入复用 `H264FileWriter` 添加四字节起始码。返回 AU 拥有自己的字符串字节，编码器继续编码、排空和关闭后仍有效。参数集 getter 返回内部引用，应在当前操作间隙读取或复制。

## 时间规则

- 编码器和输出统一采用 `1/90000` 时间基，关闭 B 帧并核对输出 DTS（有值时）等于 PTS。
- 第一帧有源 PTS：选用源时间模式，减去首帧 PTS 后转换到 90 kHz。负的源起点允许，相对输出从 0 开始；源间隔中的跳帧空档保留。
- 源时间模式中个别帧缺失 PTS：从上一输出 PTS 按本轮帧率延续；后续恢复的源 PTS 必须继续严格递增。
- 第一帧缺失 PTS：本轮固定采用按帧率生成的时间，后续即使出现源 PTS 也不切换时钟。回退帧计数使该行为可见，不把推算值伪装为源 PTS。
- 有正的源 duration 时按源时间基换算；否则用相邻帧序号的绝对有理数换算之差，避免分数帧率逐帧累加误差。duration 表示该帧时长，不承诺等于跳帧后的下一个 PTS 差。
- 源时间基变化、倒退/重复 PTS、90 kHz 取整后不再递增、负时长及数值溢出会明确报错，不静默压平时间异常。
- 编码包按 PTS 关联提交时记录的 duration；不依赖某次 `encode()` 恰好立即产生一个包。

## 验证记录（2026-11-06）

使用 BtbN FFmpeg shared `N-128433-gdb8537d8cc-20261105`（libx264）开发包，保存于本机忽略目录 `.verify/stage3-sdk/`，SDK 和媒体产物不纳入 Git。

- MSVC Debug 仅开启编码：17/17 CTest 通过，包含原有 13 项回归。
- MSVC Debug 同时开启采集/编码：20/20 CTest 通过；仅编码的最终专项 5/5 通过。默认关闭两项功能的 MinGW Debug 构建与 13/13 回归通过；MinGW Release 编码探测程序构建、BGR24 生成帧编码通过。
- 专项覆盖 YUYV422 30 fps、BGR24 25 fps、30000/1001 fps、带填充输入行、多 slice AU、首帧及周期 IDR、源时间间隔/负起点/缺失/溢出、分数帧率回退、重复打开/关闭、排空和错误输入。
- 实际调用 FFmpeg H.264 解码器核对输出尺寸、YUV420P、BT.601/limited-range 标记、逐帧 PTS、帧数和画面；彩色测试图的逐帧亮度及色度平均绝对误差须小于 6（8 bit），不要求有损编码字节或像素完全相等。另从后续 IDR 用全新解码器启动，并经现有 AUD 文件读取器核对 NALU/访问单元保持一致。
- 独立探测程序生成 1280x720、30 fps 的 300 帧文件：输出 300 个 AU，首帧 PTS=0、duration=3000；独立 FFmpeg 完整解码无错误，ffprobe 确认 h264/yuv420p、1280x720、无 B 帧、300 帧。
- `camera_probe --list` 和 `h264_encode_probe --list` 实际均枚举到 0 个视频设备，真实设备输入及物理拔出后的编码收尾仍待有设备时验收。自动测试不打开摄像头。
