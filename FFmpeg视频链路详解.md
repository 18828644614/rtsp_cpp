# FFmpeg 视频链路详解：从摄像头采集到 H.264 编码

> 面向读者：刚接触 FFmpeg，希望结合本项目理解视频采集、像素转换和编码的 C++ 学习者。  
> 核对日期：2026-11-19。内容以编写时的当前工作区代码为依据。  
> 阅读方式：先读第 1～8 节，理解设备打开与画面读取，再深入转换、编码与时间戳。  
> 文中的分辨率、像素值、时间戳和包大小示例用于教学，不代表本机摄像头实测结果。标注“简化示意”的代码省略了部分检查，不是可直接替换项目实现的完整代码。

## 目录

1. [本项目中 FFmpeg 到底负责什么](#s01)
2. [先理解视频、像素和像素格式](#s02)
3. [先分清信息来源和核心对象](#s03)
4. [入口：程序怎样建立视频链路](#s04)
5. [摄像头配置：我们定义了什么](#s05)
6. [打开摄像头：CameraCapture::open](#s06)
7. [读画面：AVPacket 如何变成 AVFrame](#s07)
8. [CameraFrame 的字段从哪里来](#s08)
9. [内存：data、linesize、引用与缓冲区](#s09)
10. [像素转换：转为 YUV420P](#s10)
11. [初始化 H.264 编码器](#s11)
12. [时间戳：PTS、time_base 与 90 kHz](#s12)
13. [提交图像，取出 H.264](#s13)
14. [整理 H264AccessUnit](#s14)
15. [工作线程、队列、停止与释放](#s15)
16. [跟踪一帧画面的完整示例](#s16)
17. [函数速查表与信息来源总表](#s17)
18. [常见误解、源码阅读与练习](#s18)

<a id="s01"></a>
## 1. 本项目中 FFmpeg 到底负责什么

### 1.1 调用的是库函数

FFmpeg 既有命令行工具，也有能被其他程序调用的库。本项目的 C++ 程序直接链接 FFmpeg 库，不是为每张图像启动一次 `ffmpeg.exe`。

```cpp
avformat_open_input(...);    // 打开输入。
av_read_frame(...);          // 读取输入数据包。
sws_scale(...);              // 转换原始图像。
avcodec_send_frame(...);     // 提交原始图像到编码器。
avcodec_receive_packet(...); // 取得编码输出。
```

源码经常这样包含头文件：

```cpp
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
```

FFmpeg 提供 C 接口，`extern "C"` 让 C++ 编译器按 C 链接规则处理这些声明。它本身不负责打开设备或转换图像。

### 1.2 视频链路使用的库

| 库 | 在本项目中的职责 | 典型接口 |
|---|---|---|
| `libavdevice` | 提供 DirectShow 设备输入 | `avdevice_register_all()` |
| `libavformat` | 管理输入、流、数据包读取 | `avformat_open_input()`、`av_read_frame()` |
| `libavcodec` | 解码输入包、编码原始图像 | `avcodec_send_packet()`、`avcodec_send_frame()` |
| `libavutil` | 帧、缓冲区、格式、时间换算、错误信息 | `av_frame_alloc()`、`av_rescale_q()` |
| `libswscale` | 原始图像格式转换及缩放 | `sws_getContext()`、`sws_scale()` |

依赖声明见 [CameraCapture.cmake](E:/MarkDown/rtsp-cpp/cmake/CameraCapture.cmake) 和 [H264Encoding.cmake](E:/MarkDown/rtsp-cpp/cmake/H264Encoding.cmake)。

### 1.3 FFmpeg 与项目的分工

FFmpeg 提供打开设备、读包、解码、转换、编码、缓冲区管理等能力。

项目决定设备和参数、调用顺序、超时、停止、时间线策略、线程与队列，以及怎样组织编码结果。后续 RTSP 请求处理、RTP 打包和网络发送也有项目自己的实现。

```text
项目提供采集要求
        ↓
FFmpeg 打开摄像头，建立输入流信息
        ↓
FFmpeg 读取设备数据包 AVPacket
        ↓
rawvideo 解码器输出原始图像 AVFrame
        ↓
项目包装成 CameraFrame
        ↓
swscale 转换为 YUV420P
        ↓
FFmpeg 调用 libx264 编码为 H.264
        ↓
项目整理为 H264AccessUnit
        ↓
项目队列 → 媒体会话 → RTP 打包与发送
```

本篇重点讲到编码结果进入队列。网络细节可以另外阅读 [RTSP完整传输流程.md](E:/MarkDown/rtsp-cpp/RTSP完整传输流程.md)。

<a id="s02"></a>
## 2. 先理解视频、像素和像素格式

### 2.1 视频可以先看成按时间排列的图像

30 fps 表示每秒约 30 张图像。1280×720 表示一张图像的像素数量。前者描述时间，后者描述空间大小。

只有宽高，还不知道内存中的字节如何变成颜色。还需要像素格式，说明颜色如何表达、数据如何排列。

### 2.2 BGR24：三个字节描述一个像素

```text
像素 0         像素 1         像素 2
B0 G0 R0       B1 G1 R1       B2 G2 R2
```

B、G、R 分别表示蓝、绿、红分量，每个分量一个字节。每像素 24 位，即 3 字节。

不考虑行填充，一张 1280×720 图像的有效像素大小为：

```text
1280 × 720 × 3 = 2,764,800 字节
```

### 2.3 YUYV422：亮度与色度

Y 可以先理解为亮度，U、V 理解为颜色差异信息。YUV 到显示颜色的解释还涉及颜色矩阵与数值范围。

YUYV422 是打包格式，典型顺序是：

```text
Y0 U0 Y1 V0 | Y2 U1 Y3 V1 | ...
```

相邻两个水平像素各有自己的 Y，共享一组 U、V。两个像素 4 字节，平均每像素 2 字节。

```text
1280 × 720 × 2 = 1,843,200 字节
```

本项目摄像头原始输入只接受 `yuyv422` 或 `bgr24`，见 [camera_capture.cpp](E:/MarkDown/rtsp-cpp/src/media/camera_capture.cpp) 的 `validate()`。

### 2.4 YUV420P：分三个平面

P 表示 planar，即分平面存放：

```text
Y 平面：每个像素一个亮度值
U 平面：每个 2×2 像素区域共享一个 U 值
V 平面：每个 2×2 像素区域共享一个 V 值
```

偶数宽高的图像有效大小：

```text
Y：width × height
U：(width/2) × (height/2)
V：(width/2) × (height/2)
```

1280×720 对应：

```text
921,600 + 230,400 + 230,400 = 1,382,400 字节
```

这是有效像素大小。实际缓冲区可能包含对齐空间。项目的编码器检查也要求宽高为偶数。

### 2.5 格式转换与编码是两步

```text
YUYV422 → YUV420P：改变原始图像的表示与采样布局
YUV420P → H.264：编码压缩图像
```

YUV420P 仍是原始像素。原始格式之间的转换也未必保留全部信息，例如 4:2:2 转 4:2:0 会减少垂直方向色度采样。

<a id="s03"></a>
## 3. 先分清信息来源和核心对象

### 3.1 始终追问信息来自哪里

| 类别 | 含义 | 例子 |
|---|---|---|
| 我们设置的要求 | 希望设备或编码器怎样工作 | 请求分辨率、目标码率、GOP |
| 输入或帧提供的信息 | 打开输入、读取帧后得到 | 流宽高、像素、行跨度、来源时间戳 |
| 计算或处理产生的结果 | 从已有数据进一步产生 | 帧序号、相对时间、转换后像素、H.264 字节 |

“FFmpeg 输出”还要继续分辨：可能是在转述输入信息，也可能是转换或编码产生的新结果。

### 3.2 项目自己的三个结构

| 类型 | 解决的问题 | 文件 |
|---|---|---|
| `CameraConfig` | 我希望摄像头提供什么 | [camera_capture.h](E:/MarkDown/rtsp-cpp/src/media/camera_capture.h) |
| `CameraFormat` | 打开后拿到了什么流信息 | 同上 |
| `CameraFrame` | 某张具体画面是什么 | [camera_frame.h](E:/MarkDown/rtsp-cpp/src/media/camera_frame.h) |

这些是本项目设计的 C++ 类型，不是 FFmpeg 自带的类型。

### 3.3 FFmpeg 核心对象

| 类型 | 初学者可以怎样理解 | 本项目变量 |
|---|---|---|
| `AVInputFormat` | 输入方式的描述，例如 DirectShow | `device` |
| `AVFormatContext` | 一个输入的管理对象 | `input` |
| `AVStream` | 输入中的一条媒体流 | `stream` |
| `AVCodecParameters` | 这条流的媒体参数 | `parameters`、`stream->codecpar` |
| `AVCodec` | 某种编解码器的描述 | `codec`、`encoder` |
| `AVCodecContext` | 本次编解码任务的参数和状态 | `decoder`、编码器的 `codec` |
| `AVPacket` | 接口间传递的一包数据 | `packet` |
| `AVFrame` | 原始图像及其属性、内存布局 | `decoded`、`converted` |
| `SwsContext` | 像素转换的配置与状态 | `converter` |
| `AVDictionary` | 参数字典 | `options` |

不要把 `AVPacket` 一律记成“压缩数据”。本项目编码输出包保存 H.264，而 rawvideo 输入包可以承载未压缩图像。

```text
AVCodec：找到 rawvideo 解码器这种处理能力。
AVCodecContext：为当前这一路视频建立独立处理任务。
```

上下文有运行状态，不能只理解为静态参数表。

<a id="s04"></a>
## 4. 入口：程序怎样建立视频链路

主要源码：

- [server_main.cpp](E:/MarkDown/rtsp-cpp/src/app/server_main.cpp)：`make_video_source()`。
- [camera_capture_input.cpp](E:/MarkDown/rtsp-cpp/src/media/camera_capture_input.cpp)：输入适配器。
- [camera_video_source.cpp](E:/MarkDown/rtsp-cpp/src/media/camera_video_source.cpp)：后台生产循环。

当前服务器将选项传给 `make_video_source()`，先建立编码配置：

```cpp
rtsp::CameraVideoSourceConfig config;
config.timeline = timeline;
config.encoder.width = options.width;
config.encoder.height = options.height;
config.encoder.fps_num = options.fps;
config.encoder.pixel_format = options.pixel_format;
config.encoder.bitrate = options.bitrate;
config.encoder.gop = options.gop;
```

真实摄像头分支还建立采集配置：

```cpp
rtsp::CameraConfig capture;
capture.device_name = options.device_name;
capture.width = options.width;
capture.height = options.height;
capture.fps_num = options.fps;
capture.pixel_format = options.pixel_format;

input = rtsp::make_camera_capture_input(capture);
```

两端请求的宽高、帧率和原始格式保持一致；目标码率与 GOP 用于后续编码。

`make_camera_capture_input()` 创建适配器，此刻还没有立即打开设备。适配器第一次 `read()` 时才执行：

```cpp
if (!opened_) {
    capture_.open(config_, &stopping_);
    opened_ = true;
}
```

`CameraVideoSource` 构造时启动工作线程，线程的 `produce()` 持续调用输入对象。

```text
主线程建立配置
    ↓
创建 CameraFrameInput 适配器
    ↓
创建 CameraVideoSource，启动工作线程
    ↓
工作线程初始化 H264Encoder
    ↓
第一次 input_->read() 触发 CameraCapture::open()
    ↓
持续采集 → 转换 → 编码 → 入队
```

当前代码也支持合成图像输入。它绕过真实摄像头采集，用于无设备场景；本篇讲解实际摄像头分支。

<a id="s05"></a>
## 5. 摄像头配置：我们定义了什么

`CameraConfig` 默认值：

```cpp
struct CameraConfig {
    std::string device_name = "Integrated Camera";
    int width = 1280;
    int height = 720;
    int fps_num = 30;
    int fps_den = 1;
    std::string pixel_format = "yuyv422";
};
```

这是请求配置。运行时命令行选项可能覆盖默认值，不能把默认值当成实测结果。

| 字段 | 请求含义 | 是否已证明设备支持 |
|---|---|---|
| `device_name` | 打开指定设备 | 否，要等查找和打开 |
| `width/height` | 使用指定分辨率 | 否，要协商并校验 |
| `fps_num/fps_den` | 使用指定帧率 | 否，要协商并校验 |
| `pixel_format` | 使用指定原始格式 | 否，要协商并校验 |

```text
fps = fps_num / fps_den
30 / 1 = 30 fps
```

FFmpeg 的 `AVRational` 也使用分子、分母，适合表达帧率和时间单位。

设备名可以手动输入，也可以从枚举结果取得。`CameraCapture::enumerate_devices()` 直接使用 Windows COM / DirectShow 接口枚举视频输入设备：

```text
枚举设备 → 得到友好名称与打开标识 → 选择设备 → 传给 FFmpeg
```

项目并不是通过解析 FFmpeg 命令行输出完成枚举的。

<a id="s06"></a>
## 6. 打开摄像头：CameraCapture::open

源码：[camera_capture.cpp](E:/MarkDown/rtsp-cpp/src/media/camera_capture.cpp)，搜索 `CameraCapture::open`。

### 6.1 重置状态并检查配置

函数先关闭旧输入、校验新配置、设置停止条件，清空错误、序号和首帧时间记录。

这是项目自己的生命周期管理。`validate(config)` 检查设备名、尺寸、帧率和支持的格式。

### 6.2 找到 DirectShow 输入组件

```cpp
avdevice_register_all();
const AVInputFormat* device = av_find_input_format("dshow");
```

- `avdevice_register_all()` 注册设备输入输出组件。
- `av_find_input_format("dshow")` 查找 DirectShow 输入组件。
- 返回的 `device` 描述一种输入方式，还不是已打开的摄像头。

`"dshow"` 是项目指定的。如果使用的 FFmpeg 构建不支持它，代码会报错。

### 6.3 把请求放入字典

```cpp
AVDictionary* options = nullptr;

av_dict_set(&options, "video_size",
    (std::to_string(config.width) + "x" +
     std::to_string(config.height)).c_str(), 0);

av_dict_set(&options, "framerate",
    (std::to_string(config.fps_num) + "/" +
     std::to_string(config.fps_den)).c_str(), 0);

av_dict_set(&options, "pixel_format",
    config.pixel_format.c_str(), 0);
```

按默认值展开，相当于：

```text
video_size   = 1280x720
framerate    = 30/1
pixel_format = yuyv422
```

项目实际还用 `require_success()` 检查设置字典的返回值，这里为突出核心调用做了省略。

这些是设备输出要求。设备需要支持这组配置，不能理解为“打开时自动把任意输入转换成要求的格式”。

### 6.4 创建输入上下文

```cpp
impl_->input = avformat_alloc_context();
```

分配成功只表示管理对象已创建，不代表设备已打开或已经读到画面。

提前创建，是为了设置输入行为：

```cpp
impl_->input->flags |= AVFMT_FLAG_NONBLOCK;
impl_->input->interrupt_callback = {Impl::interrupted, impl_.get()};
```

非阻塞标志表达读取方式，中断回调检查停止条件：

```cpp
static int interrupted(void* opaque) {
    return static_cast<Impl*>(opaque)->stopped() ? 1 : 0;
}
```

项目设定停止标记，FFmpeg 在相应中断检查点调用回调。一个标志或回调并不对所有驱动操作提供严格硬实时保证。

### 6.5 真正打开输入

```cpp
avformat_open_input(
    &impl_->input,
    ("video=" + config.device_name).c_str(),
    device,
    &options);
```

| 参数 | 含义 | 来源 |
|---|---|---|
| `&impl_->input` | 输入上下文指针的地址 | 项目持有，FFmpeg 可更新 |
| `"video=" + config.device_name` | 要打开的视频设备 | 项目拼接，设备名来自配置 |
| `device` | 使用 DirectShow | 前一步查到的组件 |
| `&options` | 参数字典地址 | 请求值来自项目 |

`&impl_->input` 是指针的地址，允许函数更新这个指针。

打开成功后，FFmpeg 已通过输入组件建立输入及流信息。解码器仍需要后续单独创建。

### 6.6 检查未消费选项

```cpp
if (av_dict_count(options) != 0) {
    throw std::runtime_error("FFmpeg did not consume all camera options");
}
av_dict_free(&options);
```

未识别的选项会留在字典。项目检查剩余项，帮助发现选项名等配置问题。

“选项被接收”和“设备实际格式符合请求”是两个检查层次，所以还要读取实际流属性。

### 6.7 查找视频流

```cpp
for (unsigned int index = 0; index < impl_->input->nb_streams; ++index) {
    if (impl_->input->streams[index]->codecpar->codec_type
            == AVMEDIA_TYPE_VIDEO) {
        impl_->stream_index = static_cast<int>(index);
        break;
    }
}
```

一个输入可能有多条流。`nb_streams` 是流数量，`streams[index]` 是某一条流。

`stream_index` 由项目保存，后面读包时用它选择目标视频数据。

### 6.8 读取实际流信息

```cpp
AVStream* stream = impl_->input->streams[impl_->stream_index];
const AVCodecParameters* parameters = stream->codecpar;

const auto pixel_format = static_cast<AVPixelFormat>(parameters->format);
const AVRational rate = av_guess_frame_rate(impl_->input, stream, nullptr);
```

| 表达式 | 含义 | 来源说明 |
|---|---|---|
| `parameters->width/height` | 输入流宽高 | FFmpeg 从输入链路建立 |
| `parameters->format` | 像素格式枚举 | 输入流参数 |
| `parameters->codec_id` | 输入数据编码类型 | 输入流参数 |
| `stream->time_base` | 输入流时间戳单位 | FFmpeg 输入流信息 |
| `av_guess_frame_rate(...)` | 根据已有信息推断帧率 | FFmpeg 辅助判断 |

这里的“实际”指打开后的协商流信息，不表示已长期测量真实到帧率、丢帧率或延迟。

### 6.9 比较请求和实际信息

项目要求 `codec_id` 为 `AV_CODEC_ID_RAWVIDEO`，再比较宽高、像素格式、帧率和时间单位。

```text
CameraConfig.width：请求的宽度
parameters->width：输入流报告的宽度
        ↓
比较，不符合就报错。
```

通过后保存到项目自己的 `CameraFormat`：

```cpp
impl_->actual = {
    parameters->width,
    parameters->height,
    rate.num,
    rate.den,
    av_get_pix_fmt_name(pixel_format),
    avcodec_get_name(parameters->codec_id),
    stream->time_base.num,
    stream->time_base.den
};
```

内容主要从流信息复制，部分枚举转换为名称。它不是简单把 `CameraConfig` 原样抄一遍。

### 6.10 创建 rawvideo 解码器

```cpp
const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
impl_->decoder = avcodec_alloc_context3(codec);
avcodec_parameters_to_context(impl_->decoder, parameters);
impl_->decoder->pkt_timebase = stream->time_base;
avcodec_open2(impl_->decoder, codec, nullptr);
```

| 操作 | 做什么 | 参数从哪里来 |
|---|---|---|
| `avcodec_find_decoder()` | 查找解码器 | 输入流的 `codec_id` |
| `avcodec_alloc_context3()` | 创建解码任务上下文 | 解码器描述 |
| `avcodec_parameters_to_context()` | 复制流参数 | 输入流参数 |
| `pkt_timebase = stream->time_base` | 指定输入包时间单位 | 输入流信息 |
| `avcodec_open2()` | 初始化解码器 | 已配置的上下文 |

原始图像虽然没有经过 H.264 压缩，仍可通过 rawvideo 解码器，使用统一接口从包得到带图像布局的帧。

这里不是 `YUYV422 → YUV420P` 的转换。像素转换由后面的编码模块另行完成。

### 6.11 分配可复用容器

```cpp
impl_->packet = av_packet_alloc();
impl_->decoded = av_frame_alloc();
```

这两句创建包和帧对象。后续读取、解码才会填充画面数据。

<a id="s07"></a>
## 7. 读画面：AVPacket 如何变成 AVFrame

源码：[camera_capture.cpp](E:/MarkDown/rtsp-cpp/src/media/camera_capture.cpp)，搜索 `CameraCapture::read`。

### 7.1 三个核心函数

```cpp
av_read_frame(input, packet);             // 输入 → 数据包。
avcodec_send_packet(decoder, packet);    // 数据包 → 解码器。
avcodec_receive_frame(decoder, decoded); // 解码器 → 原始帧。
```

`av_read_frame()` 的输出是 `AVPacket`，不是 `AVFrame`。函数名中的 frame 不应该代替你对参数类型的判断。

### 7.2 实际代码先取输出，再补输入

下面是简化示意，省略了停止、超时与错误信息的详细处理：

```cpp
for (;;) {
    int received = avcodec_receive_frame(decoder, decoded);
    if (received == 0) {
        deliver(frame);
        return FrameReady;
    }
    if (received != AVERROR(EAGAIN)) {
        return Failed;
    }

    int result = av_read_frame(input, packet);
    if (result == AVERROR(EAGAIN)) {
        // 实际项目会检查截止时间并短暂等待。
        continue;
    }
    if (result < 0) {
        return Failed;
    }

    if (packet->stream_index == stream_index) {
        int sent = avcodec_send_packet(decoder, packet);
        av_packet_unref(packet);
        if (sent < 0) {
            return Failed;
        }
    } else {
        av_packet_unref(packet);
    }
}
```

为什么先取输出：解码器内部可能还有尚未取走的结果。只有它明确需要更多输入时，项目才读取下一包。

通用解码接口不能一概理解为“一个包立刻产出一帧”。即使本项目输入较简单，也需要按接口状态推进。

### 7.3 两处 EAGAIN 不同

| 调用 | 含义 | 项目下一步 |
|---|---|---|
| `avcodec_receive_frame()` | 当前无帧可取，需要更多解码输入 | 读包并提交 |
| `av_read_frame()` | 非阻塞输入当前无可读包 | 检查超时，短暂等待后重试 |

编解码器的 EAGAIN 要通过另一侧 send/receive 操作推进，不能靠单纯等待时间解决。

当前摄像头实现先把输出取到 EAGAIN，再送下一个包，将发送返回的负值视为失败。不要把其他模块中的“发送 EAGAIN 后重试”分支当作这里已有的代码。

### 7.4 包编号用于选择视频流

```cpp
if (impl_->packet->stream_index == impl_->stream_index) {
    // 提交给目标视频解码器。
}
```

`packet->stream_index` 由输入读取填充；`impl_->stream_index` 是打开阶段保存的目标流编号。

### 7.5 read 返回项目定义的状态

| `CaptureResult` | 含义 |
|---|---|
| `FrameReady` | 成功交付一张 `CameraFrame` |
| `Timeout` | 本次尝试达到截止时间，暂未交付画面 |
| `Stopped` | 收到停止请求 |
| `Failed` | 输入或解码失败 |

这些状态由项目综合 FFmpeg 返回值、停止与超时条件得出。适配器再转换为 `CameraInputResult`，不可恢复错误则抛异常，供工作线程统一处理。

<a id="s08"></a>
## 8. CameraFrame 的字段从哪里来

源码：[camera_frame.h](E:/MarkDown/rtsp-cpp/src/media/camera_frame.h)，以及 `CameraCapture::Impl::deliver()`。

### 8.1 属性复制与项目生成混合在一起

```cpp
frame.sequence = sequence++;
frame.width = decoded->width;
frame.height = decoded->height;

const char* format_name = av_get_pix_fmt_name(
    static_cast<AVPixelFormat>(decoded->format));
frame.pixel_format = format_name ? format_name : "unknown";

std::copy_n(decoded->linesize,
    frame.linesize.size(), frame.linesize.begin());

frame.time_base_num = actual.time_base_num;
frame.time_base_den = actual.time_base_den;
frame.source_duration = decoded->duration;
frame.arrival_steady_us = steady_us();
```

### 8.2 字段来源表

| 字段 | 直接来源 | 怎样理解 |
|---|---|---|
| `buffer` | 克隆的 FFmpeg 帧 | 保留图像引用，项目包装生命周期 |
| `sequence` | `sequence++` | 项目交付帧时生成的编号 |
| `width/height` | `decoded->width/height` | 当前帧的宽高 |
| `pixel_format` | `decoded->format` 转名称 | 当前帧格式 |
| `linesize` | `decoded->linesize` | 当前帧实际行跨度 |
| `source_pts` | `best_effort_timestamp`，无值时用 `pts` | FFmpeg 输出帧提供的来源时间 |
| `time_base_num/den` | 保存的流时间单位 | 解释来源时间戳的单位 |
| `source_duration` | `decoded->duration` | 输入帧持续时间，可能没有有效正值 |
| `relative_pts_us` | 来源时间减首个有效时间后换算 | 项目计算的相对微秒时间 |
| `arrival_steady_us` | 本机 `steady_clock` | 执行交付时记录的到达相关时刻 |
| `capture_pts_us` | 后续 `CameraTimeMapper` | 映射到共同时间线的结果，有相应配置时设置 |

### 8.3 sequence 不是设备硬件帧号

```text
成功交付第一帧：0
成功交付第二帧：1
成功交付第三帧：2
```

这是项目递增的计数。设备或驱动此前即使丢过数据，项目也可能看到连续序号，不能据此证明采集从未丢帧。

### 8.4 来源时间与到达时间不同

`source_pts` 来自输入链路与 FFmpeg 输出的时间信息。未核实驱动实现时，不要直接称它为传感器硬件曝光时间。

`arrival_steady_us` 是程序执行到 `steady_us()` 时读取的本机单调时钟，也不是曝光时刻。

`relative_pts_us` 由项目计算：

```cpp
frame.relative_pts_us = av_rescale_q(
    timestamp - *first_pts,
    {actual.time_base_num, actual.time_base_den},
    {1, 1000000});
```

首个有效来源时间作为起点，结果使用微秒。它不是摄像头直接传来的现成相对微秒数。

<a id="s09"></a>
## 9. 内存：data、linesize、引用与缓冲区

### 9.1 AVFrame 描述数据及布局

```cpp
frame->width;
frame->height;
frame->format;
frame->data[0];
frame->linesize[0];
```

`data[]` 是各平面的地址，`linesize[]` 是对应行跨度。

BGR24、YUYV422 在这里使用第一个平面；YUV420P 分开保存 Y、U、V。

### 9.2 linesize 不能直接拿宽度代替

假设 BGR24 宽度 641：

```text
一行有效像素字节数 = 641 × 3 = 1923
```

缓冲区可能为对齐增加填充，所以相邻行起点的距离要读取 `linesize`。

```cpp
const uint8_t* row = frame->data[0] + y * frame->linesize[0];
```

某些布局允许负行跨度。项目编码输入检查考虑了步长的绝对大小，转换时直接传入实际步长。

外层 `CameraFrame` 虽保留步长信息，但编码器实际交给 `sws_scale()` 的是底层 `AVFrame` 的 `data` 与 `linesize`。

### 9.3 分配对象与像素缓冲区不同

```cpp
AVFrame* output = av_frame_alloc();
```

只创建帧对象。

```cpp
output->format = AV_PIX_FMT_YUV420P;
output->width = width;
output->height = height;
av_frame_get_buffer(output, 32);
```

根据格式、尺寸分配图像缓冲区，建立相应地址和行跨度。`32` 是对齐参数，不是 32 位像素格式或 32 字节总大小。

### 9.4 交付时为什么 clone

```cpp
AVFrame* retained = av_frame_clone(decoded);
frame.buffer = std::shared_ptr<AVFrame>(retained, [](AVFrame* owned) {
    av_frame_free(&owned);
});
av_frame_unref(decoded);
```

项目同时需要：采集器复用 `decoded`，外部上一帧继续有效。

对这里的引用计数缓冲区，clone 创建新帧对象并增加底层缓冲区引用，通常不复制整张图像。

```text
decoded 帧对象 ───┐
                 ├── 同一块像素缓冲区
retained 帧对象 ──┘
```

`av_frame_unref(decoded)` 解除采集器的引用，仍被外部引用的像素不会无条件销毁。外部最后一个持有者释放时，删除器再调用 `av_frame_free()`。

不能把所有可能的非引用计数输入都假设为同样的零拷贝行为；这里解释的是本项目帧路径。

### 9.5 转换前 make_writable

```cpp
av_frame_make_writable(impl_->converted);
```

编码器可能仍引用上一轮提交的图像，直接覆盖可能破坏尚未处理完的数据。

这个函数确保当前帧可安全写入：独占时可复用，共享时必要时建立独立缓冲区。它不表示每次都会重新分配整张图像。

<a id="s10"></a>
## 10. 像素转换：转为 YUV420P

源码：[h264_encoder.cpp](E:/MarkDown/rtsp-cpp/src/media/h264_encoder.cpp)，搜索 `sws_getContext`、`sws_scale`。

### 10.1 目标格式由项目选择

```cpp
codec->pix_fmt = AV_PIX_FMT_YUV420P;
```

这句告诉编码器“之后给你的图像是 YUV420P”，本身不会改变图像。

`H264EncoderConfig::pixel_format` 表示编码模块预期收到的原始输入格式；`codec->pix_fmt` 是转换后的编码输入格式。两者处于转换前后不同阶段。

### 10.2 创建转换上下文

```cpp
state->converter = sws_getContext(
    config.width, config.height, state->input_format,
    config.width, config.height, AV_PIX_FMT_YUV420P,
    SWS_BILINEAR, nullptr, nullptr, nullptr);
```

| 参数组 | 意义 | 谁决定 |
|---|---|---|
| 输入宽高、格式 | 如何解释输入图像 | 项目配置，后续校验实际帧 |
| 输出宽高、格式 | 转换目标 | 项目配置与固定规则 |
| `SWS_BILINEAR` | 转换/缩放选项 | 项目选择 |
| 返回的 `SwsContext*` | 转换任务配置与状态 | FFmpeg 创建 |

本项目输入输出尺寸相同，主要转换像素格式。这个接口本身也支持描述不同尺寸的转换。

### 10.3 明确指定颜色规则

项目使用 BT.601 系数，并将 YUYV 输入按 limited range、BGR 输入按 full range，输出按 limited range 处理：

```cpp
const int* coefficients = sws_getCoefficients(SWS_CS_ITU601);
sws_setColorspaceDetails(state->converter,
    coefficients, state->input_format == AV_PIX_FMT_BGR24 ? 1 : 0,
    coefficients, 0, 0, 1 << 16, 1 << 16);
```

这是项目的颜色约定，不是自动识别所有摄像头得到的通用结论。

对声明其他 YUYV 颜色范围或色彩空间的输入，编码器有相应拒绝检查。元数据未指定时，按当前约定解释。

### 10.4 分配输出帧

```cpp
state->converted = av_frame_alloc();
state->converted->format = AV_PIX_FMT_YUV420P;
state->converted->width = config.width;
state->converted->height = config.height;
av_frame_get_buffer(state->converted, 32);
```

建立转换结果的存放位置，像素内容需要逐帧转换得到。

### 10.5 每帧执行真正的转换

编码器先检查当前帧宽高、格式、地址和步长是否符合要求，再执行：

```cpp
av_frame_make_writable(impl_->converted);
const int rows = sws_scale(
    impl_->converter,
    input->data,
    input->linesize,
    0,
    config.height,
    impl_->converted->data,
    impl_->converted->linesize);
```

| 参数 | 含义 |
|---|---|
| `converter` | 已建立的转换配置 |
| `input->data` | 输入各平面地址 |
| `input->linesize` | 输入各平面行跨度 |
| `0` | 从输入第 0 行开始 |
| `config.height` | 输入片段覆盖整张图像 |
| `converted->data` | 输出各平面地址 |
| `converted->linesize` | 输出各平面行跨度 |

返回值是输出行数，项目检查是否等于 `config.height`。

结果是新的 YUV420P 像素，FFmpeg 根据输入和规则计算它们。这里没有再次访问摄像头。

<a id="s11"></a>
## 11. 初始化 H.264 编码器

### 11.1 项目定义编码配置

源码：[h264_encoder.h](E:/MarkDown/rtsp-cpp/src/media/h264_encoder.h)。

```cpp
struct H264EncoderConfig {
    int width = 1280;
    int height = 720;
    int fps_num = 30;
    int fps_den = 1;
    int bitrate = 2000000;
    int gop = 30;
    std::string pixel_format = "yuyv422";
    bool use_capture_timeline = false;
};
```

运行时选项可以覆盖默认配置。

摄像头原始输入带宽和 H.264 目标码率不同。`2000000` 是后续编码的目标 bit/s，不是从原始摄像头读取的 H.264 属性。

### 11.2 主动选择 libx264

```cpp
const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
state->codec = avcodec_alloc_context3(encoder);
```

项目要求使用的 FFmpeg 构建提供 libx264。FFmpeg 提供统一编码接口，实际 H.264 编码由这个编码器实现完成。

### 11.3 填写编码上下文

```cpp
codec->width = config.width;
codec->height = config.height;
codec->pix_fmt = AV_PIX_FMT_YUV420P;
codec->time_base = media_time_base; // 本文件定义为 {1, 90000}。
codec->framerate = {config.fps_num, config.fps_den};
codec->bit_rate = config.bitrate;
codec->gop_size = config.gop;
codec->max_b_frames = 0;
codec->thread_count = 2;
```

| 字段 | 意义 | 来源 |
|---|---|---|
| `width/height` | 编码输入分辨率 | 我们的配置 |
| `pix_fmt` | 原始输入为 YUV420P | 项目固定选择 |
| `time_base` | 时间戳单位为 1/90000 秒 | 项目固定选择 |
| `framerate` | 配置帧率 | 我们的配置 |
| `bit_rate` | 目标 bit/s | 我们的配置 |
| `gop_size` | GOP/关键帧间隔相关配置 | 我们的配置 |
| `max_b_frames` | 不使用 B 帧 | 项目设为 0 |
| `thread_count` | 线程数量配置 | 项目设为 2 |

设置 30/1 不会生成缺失画面，也不证明实际采集始终稳定为 30 fps。

目标码率不保证每包大小相同或每秒输出字节严格相同。图像内容和编码决策都会影响输出。

### 11.4 编码器专用选项

```cpp
av_dict_set(&options, "preset", "veryfast", 0);
av_dict_set(&options, "tune", "zerolatency", 0);
av_dict_set(&options, "profile", "baseline", 0);
av_dict_set(&options, "x264-params",
    "annexb=1:repeat-headers=1:aud=1:scenecut=0:open-gop=0", 0);
```

| 选项 | 用途 |
|---|---|
| `preset=veryfast` | 选择偏重速度的预设 |
| `tune=zerolatency` | 针对低延迟用途调整编码 |
| `profile=baseline` | 选择 Baseline 配置 |
| `annexb=1` | 请求 Annex-B 字节流 |
| `repeat-headers=1` | 请求在相应位置重复参数集 |
| `aud=1` | 请求访问单元分隔符，配合项目解析约定 |
| `scenecut=0` | 禁用场景切换驱动的相关关键帧判定 |
| `open-gop=0` | 禁用开放 GOP |

低延迟编码设置不会消除设备缓冲、队列、网络和播放器的延迟。

### 11.5 打开编码器

```cpp
avcodec_open2(codec, encoder, &options);
```

前面在准备参数，这里初始化编码器。项目随后检查顶层未消费选项，但这不等于测量了所有 x264 内部行为。

<a id="s12"></a>
## 12. 时间戳：PTS、time_base 与 90 kHz

第一次阅读可先知道“图像除了像素还需要时间信息”，第二遍再深入策略。

### 12.1 PTS 必须结合单位

PTS 可以先理解为图像应在媒体时间线上何时呈现。

```text
时间（秒）= pts × time_base.num / time_base.den

pts = 500，time_base = 1/1000 → 0.5 秒
pts = 45000，time_base = 1/90000 → 0.5 秒
```

不能忽略单位和起点，直接比较不同流的整数时间戳。

### 12.2 90 kHz 不是 90000 fps

```cpp
constexpr AVRational media_time_base{1, 90000};
```

时间戳每加 1，代表 1/90000 秒。对于理想固定 30 fps：

```text
每帧间隔 1/30 秒 = 3000 个 90 kHz 时间单位

第 0 帧：0
第 1 帧：3000
第 2 帧：6000
```

采用来源时间或共同时间线时，要看实际映射结果，不能假设所有帧都严格增加 3000。

### 12.3 av_rescale_q 换算时间单位

```cpp
av_rescale_q(value, source_time_base, target_time_base);
```

保持所代表的时间，换一种整数单位表达。例如 500 个毫秒单位：

```text
500 × (1/1000) ÷ (1/90000) = 45000
```

整数换算还涉及舍入，并非所有分数帧间隔都能精确表示。

### 12.4 本项目的时间戳模式

源码：`H264Encoder::Impl::timing()`。

| 模式 | 来源 | 处理 |
|---|---|---|
| `Source` | 输入有来源 PTS | 减来源起点，换算为 90 kHz |
| `FrameRate` | 建立模式时无来源 PTS | 按提交帧数和配置帧率推算 |
| `CaptureTimeline` | 配置共同采集时间线 | 将 `capture_pts_us` 换算为 90 kHz |
| `Unset` | 普通模式尚未建立 | 根据初始输入确定模式 |

来源模式中，某个后续帧暂时缺 PTS 时，代码可在上一帧基础上按名义步长推进。这是项目回退结果，不是设备提供的时间。

```cpp
pts = av_rescale_q(
    *frame.source_pts - origin,
    {frame.time_base_num, frame.time_base_den},
    media_time_base);
```

### 12.5 共同时间线与 capture_pts_us

当前服务器同时建立实时视频与音频源时，可创建共享 `CaptureTimeline`，传给两个源。这里仅解释视频部分。

源码：[capture_timeline.h](E:/MarkDown/rtsp-cpp/src/media/capture_timeline.h) 和 [camera_video_source.cpp](E:/MarkDown/rtsp-cpp/src/media/camera_video_source.cpp) 的 `CameraTimeMapper`。

```text
首帧：用到达相关时刻估计共同时间线上的起点。
后续：首帧有来源 PTS 时，主要使用来源 PTS 的增量推进。
```

这避免把每帧到达抖动都直接写入时间线。首帧没有来源 PTS 的路径则使用到达相关时间映射。

```cpp
frame.capture_pts_us = mapper.map(frame, *config_.timeline);
```

编码器随后换算单位：

```cpp
pts = av_rescale_q(*frame.capture_pts_us,
    {1, 1000000}, media_time_base);
```

`relative_pts_us` 是来源首个有效时间归零后的结果；`capture_pts_us` 是共同时间线上的结果，起点不同。

### 12.6 项目写入编码输入时间

```cpp
impl_->converted->pts = timing.pts;
impl_->converted->duration = timing.duration;
```

这不是 `sws_scale()` 自动设置的。项目选择策略并进行计算。

来源提供正持续时间时换算 `duration`，否则使用名义帧间隔；还会检查时间戳递增和数值范围。

<a id="s13"></a>
## 13. 提交图像，取出 H.264

### 13.1 编码和解码方向相反

```text
解码：AVPacket → 解码器 → AVFrame
编码：AVFrame  → 编码器 → AVPacket
```

```cpp
// 解码。
avcodec_send_packet(decoder, packet);
avcodec_receive_frame(decoder, frame);

// 编码。
avcodec_send_frame(encoder_context, frame);
avcodec_receive_packet(encoder_context, packet);
```

两种上下文都是 `AVCodecContext*`，区别在于打开哪种编解码器、用于什么任务。

### 13.2 提交 YUV420P 图像

源码：`H264Encoder::encode()`。

```cpp
int sent = avcodec_send_frame(impl_->codec, impl_->converted);
if (sent == AVERROR(EAGAIN)) {
    impl_->receive(output);
    sent = avcodec_send_frame(impl_->codec, impl_->converted);
}
```

成功表示编码器接受输入，不等于这次调用已经返回 H.264 字节。

发送返回 EAGAIN 时，项目先取出已有输出，再重试同一张图像。

### 13.3 保存待输出时间记录

```cpp
impl_->pending.emplace(timing.pts, timing.duration);
```

`pending` 是项目自己的映射表，把后续编码包与已提交图像的时间对应起来。它不是 FFmpeg 输入队列，不保存整张像素。

### 13.4 循环接收编码结果

源码：`H264Encoder::Impl::receive()`。

```cpp
for (;;) {
    int result = avcodec_receive_packet(codec, packet);
    if (result == AVERROR(EAGAIN)) {
        return false;
    }
    if (result == AVERROR_EOF) {
        return true;
    }
    // 检查错误与时间，解析 packet，保存项目输出。
}
```

上面是简化示意，实际代码使用 `require_success()` 检查其他负返回值。

| 字段 | 含义 | 来源 |
|---|---|---|
| `packet->data` | H.264 字节地址 | 编码器输出 |
| `packet->size` | 本次输出字节数 | 编码器输出 |
| `packet->pts` | 呈现时间戳 | 编码器根据输入时间等输出 |
| `packet->dts` | 解码时间戳 | 编码器输出 |

项目没有使用 B 帧，并校验输出 PTS 与待输出记录匹配、已提供的 DTS 与 PTS 一致。

通用编码 API 不保证“提交一帧立即得到一个包”。本项目通过特定 libx264 配置与解析校验，进一步要求一个编码包恰好对应一个带 AUD 的图像访问单元。

### 13.5 为什么 encode 返回 vector

```cpp
std::vector<H264AccessUnit> H264Encoder::encode(const CameraFrame& frame);
```

输入接收与输出获取是分离的，这次调用收集当前可取到的输出。集合可能为空，也可能包含本轮取出的结果。

不能仅凭 `encode(frame)` 这个名字，推断任何编码器都立刻返回唯一一帧字节。

<a id="s14"></a>
## 14. 整理 H264AccessUnit

### 14.1 FFmpeg 包与项目对象不同

后续媒体会话使用的结构定义在 [h264_access_unit.h](E:/MarkDown/rtsp-cpp/src/media/h264_access_unit.h)：

```cpp
struct H264AccessUnit {
    std::vector<std::string> nalus;
    bool is_keyframe = false;
    std::int64_t pts = 0;
    std::uint32_t duration = 0;
};
```

AU 在这里可以先理解为组成一张编码图像及相关信息的一组 NALU。一张图像可包含多个 slice，因此 AU 不等于单个 NALU。

### 14.2 Annex-B 与 NALU

项目请求 Annex-B 输出，使用起始码分隔 NALU，例如：

```text
00 00 00 01 + NALU
00 00 01    + NALU
```

`parse_packet()` 查找起始码，校验 NALU 头，把 NALU 字节保存到 `unit.nalus`。

| 类型 | 名称 | 在本项目中的用途 |
|---|---|---|
| 1 | 非 IDR 图像 slice | 普通图像数据 |
| 5 | IDR 图像 slice | 设置关键帧标记 |
| 7 | SPS | 缓存序列参数集 |
| 8 | PPS | 缓存图像参数集 |
| 9 | AUD | 校验访问单元边界约定 |

保存后的 NALU 包含 NAL 头与负载，不包含 Annex-B 起始码。

### 14.3 关键帧标记由项目解析得出

```cpp
const auto type = header & 0x1f;
unit.is_keyframe = unit.is_keyframe || type == 5;
```

编码内容来自编码器，布尔字段由项目解析生成。这里 `is_keyframe` 明确表示含 IDR，不要把其他系统的不同定义直接套进来。

### 14.4 参数集由编码器产生，项目负责缓存

`parameter_sets()` 从输出缓存 SPS、PPS。IDR 缺少对应参数集时，项目根据已有缓存补齐。

编码参数字节来自编码器；识别、缓存、组织及后续轨道描述由项目处理。

### 14.5 为什么可以释放编码包

`parse_packet()` 把 NALU 字节保存到独立 `std::string`，项目输出有自己的字节存储。

```cpp
auto unit = parse_packet(*packet);
output.push_back(std::move(unit));
av_packet_unref(packet);
```

解除包当前的数据引用，不会销毁已复制到访问单元中的字节。这里存在编码字节复制，不能笼统称为整条链路端到端零拷贝。

<a id="s15"></a>
## 15. 工作线程、队列、停止与释放

### 15.1 produce 串联处理

源码：[camera_video_source.cpp](E:/MarkDown/rtsp-cpp/src/media/camera_video_source.cpp)。核心逻辑简化示意：

```cpp
H264Encoder encoder;
auto encoder_config = config_.encoder;
encoder_config.use_capture_timeline = static_cast<bool>(config_.timeline);
encoder.open(encoder_config);

while (!stopping_.load()) {
    CameraFrame frame;
    auto result = input_->read(frame, config_.read_timeout);

    if (result == CameraInputResult::Waiting) {
        // 当前实现还检查持续无数据是否超过 no_data_timeout。
        continue;
    }
    if (result == CameraInputResult::Ended) {
        publish(encoder.flush(), encoder);
        break;
    }
    if (config_.timeline) {
        frame.capture_pts_us = mapper.map(frame, *config_.timeline);
    }
    publish(encoder.encode(frame), encoder);
}
```

### 15.2 队列存储编码结果

`VideoFrameQueue` 保存 `H264AccessUnit`，不是原始像素。

```text
采集/编码工作线程 → H264AccessUnit 队列 → 媒体会话/发送侧
```

队列连接生产和消费、管理缓存；不替代转换或编码。

`publish()` 在获得可入队的 IDR、有效 SPS/PPS 后发布启动完成状态，所以不是只要线程启动就立即认为视频可播放。

### 15.3 分清几种期限

| 条件 | 用途 |
|---|---|
| `startup_timeout` | 等待启动后的首次可用编码信息 |
| `read_timeout` | 一次读取尝试的等待时间 |
| `no_data_timeout` | 连续未取得原始画面的故障判断 |
| 停止标记/中断 | 用户退出或对象停止时协调结束 |

单次超时可以只是暂时无帧，工作循环可继续。连续无数据超过限制，当前实现会报告故障。

### 15.4 自然输入结束时排空编码器

```cpp
avcodec_send_frame(codec, nullptr);
```

空帧不是黑色图像，而是通知编码器没有更多输入，开始排空。

`H264Encoder::flush()` 接收剩余输出，并检查待输出记录是否排空。

当前实现有一个区别：输入自然返回 `Ended` 的分支调用 `flush()`；显式停止分支直接退出循环。`close()` 与析构清理不隐式保证输出尚未排空的数据。

“释放资源”与“完整保留尾部编码输出”是不同操作。

### 15.5 释放函数的含义

| 函数 | 作用 |
|---|---|
| `av_packet_unref(packet)` | 解除包当前的数据引用，保留包对象复用 |
| `av_frame_unref(frame)` | 解除帧当前引用并重置内容，保留帧对象复用 |
| `av_packet_free(&packet)` | 释放包对象及当前引用，并置空指针 |
| `av_frame_free(&frame)` | 释放帧对象及当前引用，并置空指针 |
| `avcodec_free_context(&context)` | 释放编解码上下文 |
| `avformat_close_input(&input)` | 关闭输入并释放相应上下文 |
| `sws_freeContext(converter)` | 释放转换上下文 |
| `av_dict_free(&options)` | 释放参数字典 |

解除一个引用不等于无条件销毁其他对象仍在引用的底层数据。

摄像头 `close()` 清理解码帧、包、解码器、输入；编码器 `release()` 清理编码包、转换帧、转换器和编码上下文。

### 15.6 request_stop 和 close 不同

`request_stop()` 设置停止标记、通知等待者，让输入中断检查观察到停止。

工作线程退出并停止使用上下文后，再回收线程并完成资源清理。不要让一个线程还在读设备时，另一个线程随意释放相关上下文。

本项目适配器把摄像头打开、读取、关闭放在同一工作线程中，所有者侧通过停止请求协调退出。

<a id="s16"></a>
## 16. 跟踪一帧画面的完整示例

假设请求如下，数值用于教学：

```text
摄像头：某个已选择的设备
分辨率：1280×720
帧率：30/1
原始格式：yuyv422
H.264 目标码率：2,000,000 bit/s
```

### 16.1 提出采集要求

```cpp
capture.width = 1280;
capture.height = 720;
capture.fps_num = 30;
capture.fps_den = 1;
capture.pixel_format = "yuyv422";
```

来自我们的配置。

### 16.2 取得并校验输入流信息

假设 FFmpeg 打开设备后得到：

```text
codec_id = RAWVIDEO
width = 1280
height = 720
format = YUYV422
推断帧率 = 30/1
```

项目与请求比较，通过后保存到 `CameraFormat`。`time_base` 要从流读取，不能预设等于 `1/fps`。

### 16.3 读取设备包

```cpp
av_read_frame(input, packet);
```

取得输入数据、所属流编号及可能存在的时间信息。

### 16.4 解码器交付原始帧

```cpp
avcodec_send_packet(decoder, packet);
avcodec_receive_frame(decoder, decoded);
```

`decoded` 描述当前 YUYV422 图像。具体步长从帧读取，不猜测 `linesize`。

### 16.5 包装 CameraFrame

```text
buffer          → 持有图像引用
width/height    → 从 decoded 复制
pixel_format    → 从枚举转名称
sequence        → 项目递增
arrival time    → 项目记录本机时钟
source_pts      → 从帧时间字段读取
relative_pts_us → 项目换算
```

### 16.6 转换像素

```cpp
sws_scale(...);
```

得到同宽高的 YUV420P。该尺寸的有效像素为 1,382,400 字节，实际分配还考虑对齐与布局。

### 16.7 设置编码时间

假设使用理想固定 30 fps 的帧率推算模式，第二张提交图像可以使用 PTS 3000，单位为 1/90000 秒。

这是项目按帧计数生成的示例，不是所有设备模式下的必然值。

### 16.8 编码器产生 H.264

```cpp
avcodec_send_frame(codec, converted);
avcodec_receive_packet(codec, packet);
```

`packet->size` 是实际输出字节数，由画面与编码行为决定，不是原始大小除以固定倍数。

### 16.9 整理并入队

```text
解析 Annex-B 起始码
    ↓
提取 NALU、识别 IDR、缓存 SPS/PPS
    ↓
填写 H264AccessUnit 时间
    ↓
publish() 放入 VideoFrameQueue
```

此后模块拿到的是压缩访问单元，不再是用于像素转换的原始图像。

<a id="s17"></a>
## 17. 函数速查表与信息来源总表

### 17.1 输入和解码

| 函数 | 主要输入 | 得到什么或改变什么 |
|---|---|---|
| `avdevice_register_all()` | 项目发起调用 | 注册设备组件 |
| `av_find_input_format("dshow")` | 项目指定名称 | 输入组件描述 |
| `av_dict_set()` | 项目提供键和值 | 请求参数字典 |
| `avformat_alloc_context()` | 项目发起分配 | 输入上下文 |
| `avformat_open_input()` | 设备名、组件、配置 | 打开输入、建立流信息 |
| `av_guess_frame_rate()` | 已建立的输入流 | 根据已有信息推断帧率 |
| `avcodec_find_decoder()` | 输入流 codec_id | 解码器描述 |
| `avcodec_alloc_context3()` | 编解码器描述 | 本次编解码上下文 |
| `avcodec_parameters_to_context()` | 输入流参数 | 配置解码上下文 |
| `avcodec_open2()` | 已准备的上下文 | 初始化编解码器 |
| `av_read_frame()` | 已打开输入 | 当前数据包 |
| `avcodec_send_packet()` | 输入包 | 解码器接收输入 |
| `avcodec_receive_frame()` | 解码器状态 | 当前可取得的原始帧 |

### 17.2 图像处理与编码

| 函数 | 主要输入 | 得到什么或改变什么 |
|---|---|---|
| `av_frame_alloc()` | 分配请求 | 空帧对象 |
| `av_frame_get_buffer()` | 已填尺寸、格式等 | 像素缓冲区及布局 |
| `av_frame_clone()` | 已有帧 | 新帧对象，引用已有数据或按需复制 |
| `av_frame_make_writable()` | 待修改帧 | 确保缓冲区可安全写入 |
| `av_get_pix_fmt()` | 格式名称 | 像素格式枚举 |
| `av_get_pix_fmt_name()` | 格式枚举 | 名称字符串 |
| `sws_getContext()` | 输入输出尺寸与格式 | 转换上下文 |
| `sws_setColorspaceDetails()` | 项目选定的矩阵与范围 | 转换颜色规则 |
| `sws_scale()` | 原始像素及布局 | 转换后像素 |
| `avcodec_find_encoder_by_name()` | 项目指定 libx264 | 编码器描述 |
| `av_rescale_q()` | 原值与两个时间单位 | 换算后的整数值 |
| `avcodec_send_frame()` | 图像及项目设置的时间 | 编码器接收输入 |
| `avcodec_receive_packet()` | 编码器状态 | H.264 包 |
| `av_strerror()` | FFmpeg 错误码 | 可读错误描述 |

### 17.3 信息来源总表

| 信息 | 我们设置 | 从输入/帧读取 | 计算或处理后生成 |
|---|---|---|---|
| 请求设备 | 是，可选择枚举结果 | 枚举可读设备标识 | 项目做选择 |
| 请求宽高、帧率 | 是 | — | — |
| 请求像素格式 | 是 | — | — |
| 输入流宽高、格式 | 受请求影响，仍需检查 | 是 | FFmpeg 建立流信息 |
| 帧率判断 | 请求影响设备配置 | 基于流等信息 | FFmpeg 推断，不是到帧率实测 |
| 原始像素 | — | 是 | 经解码接口交付 |
| 行跨度 | 不能用宽度代替 | 帧布局提供 | 分配或解码时建立 |
| 来源 PTS | — | 是 | 输入链路与 FFmpeg 的时间信息 |
| 到达相关时间 | — | — | 项目读取本机时钟 |
| 帧序号 | — | — | 项目递增 |
| 相对微秒时间 | — | — | 项目换算 |
| 共同时间线 PTS | 项目选择策略 | 使用已有时间信息 | 项目映射 |
| 转换输出 YUV420P | 是 | — | swscale 计算新像素 |
| H.264 目标码率、GOP | 是 | — | 实际包大小由编码过程产生 |
| H.264 字节 | — | 不是从本项目原始摄像头直接读出 | 编码器产生 |
| SPS/PPS 字节 | 编码配置影响内容 | 从编码输出识别 | 编码器产生，项目缓存 |
| `is_keyframe` | — | 项目解析码流 | 项目判断是否含 IDR |

<a id="s18"></a>
## 18. 常见误解、源码阅读与练习

### 18.1 常见误解

**配置写了 1280×720，就证明已读到这种画面？**

配置只是要求。要看打开后的流信息、检查结果和当前帧属性。

**赋值 `pix_fmt = YUV420P`，就完成了转换？**

赋值声明格式，实际像素转换由 `sws_scale()` 完成。

**`av_read_frame()` 输出 AVFrame？**

输出是 `AVPacket`，本项目通过 rawvideo 解码器取得 `AVFrame`。

**使用解码器，输入就必然是压缩视频？**

本项目要求 rawvideo，仍使用解码接口交付原始帧。

**YUV420P 就是 H.264？**

前者是原始像素格式，后者是压缩编码格式。

**30 fps 和 90 kHz 是一回事？**

前者描述图像频率，后者在这里描述时间戳计数单位。

**帧序号连续就证明没有丢帧？**

这里的编号在项目交付时自己生成。

**到达时间就是曝光时间？**

项目记录时已经可能经过设备、驱动和输入缓冲，不等于曝光时刻。

**每次 send 都立即对应唯一一次输出？**

需要按状态循环推进。项目对特定编码输出的约定也需要单独校验。

**目标码率 2 Mbit/s，每帧大小就相同？**

目标码率用于编码控制，帧大小还取决于图像内容、帧类型和编码决策。

**所有阶段都是零拷贝？**

采集 clone 通常共享像素引用；项目解析编码包时会复制 NALU 字节。需要分阶段判断。

**close 就等于 flush？**

当前实现排空需要 `flush()`，关闭资源与保留剩余输出不同。

### 18.2 分三遍阅读

第一遍跟踪对象和方向：

```text
CameraConfig
    → AVFormatContext / AVStream
    → AVPacket
    → AVFrame
    → CameraFrame
    → YUV420P AVFrame
    → H.264 AVPacket
    → H264AccessUnit
```

第二遍看每个赋值：左边是什么，右边来自配置、输入还是计算。

第三遍深入 EAGAIN、超时、停止、时间戳、引用计数、异常清理。

### 18.3 源码定位表

用文件与函数名定位，避免代码修改后固定行号过时。

| 顺序 | 文件 | 搜索符号 | 重点 |
|---|---|---|---|
| 1 | [server_main.cpp](E:/MarkDown/rtsp-cpp/src/app/server_main.cpp) | `make_video_source` | 选项到配置 |
| 2 | [camera_capture.h](E:/MarkDown/rtsp-cpp/src/media/camera_capture.h) | `CameraConfig`、`CameraFormat` | 请求与流信息 |
| 3 | [camera_capture_input.cpp](E:/MarkDown/rtsp-cpp/src/media/camera_capture_input.cpp) | `if (!opened_)` | 第一次读时打开 |
| 4 | [camera_capture.cpp](E:/MarkDown/rtsp-cpp/src/media/camera_capture.cpp) | `CameraCapture::open` | 输入、流、解码器 |
| 5 | 同上 | `CameraCapture::read` | 包到帧、状态推进 |
| 6 | 同上 | `void deliver` | 字段来源与保活 |
| 7 | [camera_frame.h](E:/MarkDown/rtsp-cpp/src/media/camera_frame.h) | `CameraFrame` | 图像与时间 |
| 8 | [h264_encoder.cpp](E:/MarkDown/rtsp-cpp/src/media/h264_encoder.cpp) | `H264Encoder::open` | 编码器、转换器、缓冲区 |
| 9 | 同上 | `Timing timing` | 时间策略 |
| 10 | 同上 | `H264Encoder::encode` | 检查、转换、提交 |
| 11 | 同上 | `bool receive`、`parse_packet` | 取输出与整理 |
| 12 | [camera_video_source.cpp](E:/MarkDown/rtsp-cpp/src/media/camera_video_source.cpp) | `produce`、`publish` | 工作线程和入队 |

### 18.4 复习问题

1. `config.width` 和 `parameters->width` 为何都表示宽度却仍要比较？
2. 输入上下文、编解码上下文、原始帧分别管理什么？
3. `av_read_frame()` 填充什么对象？
4. 为什么先尝试 `avcodec_receive_frame()`？
5. 一行有效字节数和 `linesize` 必然相同吗？
6. YUYV422 在采集阶段就会变成 YUV420P 吗？真正转换在哪里？
7. `sws_getContext()` 和 `sws_scale()` 分别在何时调用？
8. `sequence`、`source_pts`、`arrival_steady_us` 来自哪里？
9. 编码器的时间单位为什么不能等同于摄像头帧率？
10. `avcodec_send_frame()` 成功后，在哪里取编码字节？
11. 保存的 NALU 是否包含 Annex-B 起始码？
12. `av_packet_unref()` 后，已整理的访问单元为什么仍有效？
13. 显式停止与自然结束的排空行为是否完全相同？

每次选一个问题，找到代码，再用自己的话解释输入、处理和输出。遇到新变量时，把它放回“配置、输入信息、处理结果”三类，逐步建立独立跟踪代码的能力。
