# FFmpeg 音频链路详解：从麦克风采集到 AAC 编码

> 适合读者：刚接触 FFmpeg、PCM、重采样和 AAC，希望结合本项目理解音频处理的 C++ 学习者。  
> 核对日期：2026-11-19，以编写时当前工作区的实现为依据。  
> 阅读范围：真实麦克风打开、PCM 读取、格式转换、FIFO 组帧、AAC 编码、时间线映射、编码结果入队，以及与 RTP 的衔接。  
> 所有示例数值都用于教学，不代表本次对硬件进行实测。简化代码为了突出原理，省略部分检查，不能直接替换完整实现。

本文与 [FFmpeg视频链路详解.md](E:/MarkDown/rtsp-cpp/FFmpeg视频链路详解.md) 配套。先看懂“数据是什么、参数来自哪里”，再深入线程和时间戳，不必一次记住全部函数。

## 目录

1. [先看整条音频链路](#a01)
2. [声音、采样与 PCM 是什么](#a02)
3. [四个必须分清的音频参数](#a03)
4. [音频帧、采样和字节数有什么关系](#a04)
5. [核心对象与源码地图](#a05)
6. [服务器怎样建立麦克风输入](#a06)
7. [打开麦克风：MicrophoneCapture::open](#a07)
8. [读取 PCM：包怎样变成原始音频帧](#a08)
9. [AudioFrame 各字段来自哪里](#a09)
10. [音频内存：extended_data、平面与缓冲区](#a10)
11. [先准备 AAC 编码器，确定输出要求](#a11)
12. [重采样、采样格式和声道转换](#a12)
13. [为什么转换之后还要 FIFO](#a13)
14. [按 1024 个采样构造编码输入](#a14)
15. [提交 PCM，取出 AAC 编码结果](#a15)
16. [时间戳、编码延迟与填充](#a16)
17. [共同时间线、线程与编码后队列](#a17)
18. [结束、排空、停止与释放](#a18)
19. [跟踪一段音频的完整示例](#a19)
20. [AAC 轨道描述与 RTP 衔接](#a20)
21. [函数速查表与信息来源总表](#a21)
22. [视频对照、常见误解与学习练习](#a22)

<a id="a01"></a>
## 1. 先看整条音频链路

最重要的一点：**麦克风采集出来的 PCM、转换后的 PCM、编码后的 AAC，是三个不同阶段的数据。**

~~~text
麦克风与 Windows 音频驱动
        │
        │ FFmpeg DirectShow 输入
        ▼
AVPacket：从设备读到的一包输入数据
        │
        │ PCM 解码器
        ▼
AVFrame / AudioFrame：原始 PCM
采样率、声道布局、采样格式取决于实际采集结果
        │
        │ libswresample
        ▼
48000 Hz、单声道、符合 AAC 编码器要求的 PCM
        │
        │ AVAudioFifo：暂存并重新组织采样
        ▼
每帧 1024 个采样的编码输入 AVFrame
        │
        │ FFmpeg 原生 AAC 编码器
        ▼
AVPacket：压缩后的裸 AAC 数据
        │
        │ 项目复制字节并保留包时间信息
        ▼
AacAccessUnit
        │
        │ 映射共同时间线，放进 AudioFrameQueue
        ▼
媒体会话 → AAC RTP 打包 → 网络发送
~~~

其中，FFmpeg 主要提供：

| 库 | 本项目中的作用 |
|---|---|
| libavdevice | 提供 DirectShow 设备输入 |
| libavformat | 管理输入和流，读取输入包 |
| libavcodec | PCM 解码、AAC 编码 |
| libavutil | 帧、采样格式、声道布局、FIFO、时间与错误工具 |
| libswresample | 采样率、采样格式、声道混合转换 |

项目自己决定输入选择、编码参数、调用顺序、超时、停止、组帧策略、时间线和队列。依赖声明可在 [CMakeLists.txt](E:/MarkDown/rtsp-cpp/CMakeLists.txt) 中搜索 require_ffmpeg 和 target_link_libraries。

**AAC 编码不是重采样，重采样也不是网络打包。** 后续看到函数时，先确认它位于上述哪个阶段。

<a id="a02"></a>
## 2. 声音、采样与 PCM 是什么

### 2.1 声音如何变成一串数字

可以先把麦克风接收到的声音理解为随时间变化的信号。采集系统在一系列时刻测量信号，用数字表示这些测量值。

~~~text
时刻：t0     t1     t2      t3      t4
数值：0.00   0.10   0.25   -0.08   -0.30
~~~

这些数字称为采样值。上面是概念示意，不意味着麦克风一定输出浮点数。

PCM 可以先理解为直接表达这些采样值的数字音频。在本项目里，采集层要求输入编码名称以 pcm_ 开头，之后把它交给对应 PCM 解码器，得到可按采样访问的原始帧。

### 2.2 PCM 与 AAC 的区别

~~~text
PCM：直接按格式保存采样值，便于后续计算和转换。
AAC：压缩编码后的数据，需要解码后才能得到可播放的原始采样。
~~~

项目不会把 PCM 数字直接塞进 AAC RTP 负载并称为 AAC。它会真正调用 AAC 编码器。

PCM 也不等于某一种固定格式。例如下面都是不同的 PCM 表示：

- 44100 Hz、双声道、16 位整数。
- 48000 Hz、单声道、浮点采样。
- 48000 Hz、双声道、分平面整数采样。

理解 PCM 时，还要读清楚采样率、布局与采样格式。

### 2.3 三种“码率”别混淆

举例：44100 Hz、双声道、每个采样 16 位的有效 PCM 比特率是：

~~~text
44100 × 2 × 16 = 1,411,200 bit/s
~~~

AAC 配置默认目标为 64000 bit/s，属于**后续压缩编码的控制目标**。网络实际传输还会包含 RTP、RTSP/TCP 等协议开销。

这三个数处在不同层面，不能看到配置 64000 就认为麦克风只产生每秒 64000 位的原始数据。

<a id="a03"></a>
## 3. 四个必须分清的音频参数

### 3.1 sample_rate：采样率

48000 Hz 表示每秒、每声道有 48000 个采样位置。

它不是每秒 48000 个字节，也不是每秒 48000 个音频包。

44100 → 48000 的重采样，是改变单位时间内的采样数量并计算对应的新采样值，不是只修改字段，也不是让声音简单加速播放。libswresample 提供这类重采样能力。[F1]

### 3.2 channels 与 channel layout：声道数量和布局

单声道是一条采样序列；立体声通常有左、右两条同步序列。

~~~text
左声道：L0 L1 L2 L3 ...
右声道：R0 R1 R2 R3 ...
~~~

声道数说明有几条，声道布局还说明各条代表什么及其顺序。项目使用 AVChannelLayout 保存布局，不能在多声道场景里只看一个 channels 整数。

双声道转换为单声道属于声道混合。当前代码把输入布局交给 swresample，使用它的转换规则，没有手动实现“只取左声道”，也没有显式指定自己的混合矩阵。

### 3.3 sample_format：一个采样怎样存

~~~text
s16：16 位有符号整数，通常每个分量 2 字节。
flt：32 位浮点，每个分量 4 字节。
s16p：16 位整数，分平面。
fltp：32 位浮点，分平面。
~~~

其中 p 表示 planar，即分平面。没有 p 的这些格式是 packed，即声道交错存放。格式定义与字节数由 FFmpeg 的采样格式接口提供。[F2]

双声道示意：

~~~text
packed：
一个平面：[L0 R0 L1 R1 L2 R2 ...]

planar：
平面 0：[L0 L1 L2 ...]
平面 1：[R0 R1 R2 ...]
~~~

单声道时，两种数据排列看起来相近，但格式枚举依旧不同，需要匹配编码器支持的格式。

浮点格式也仍然可以表达原始 PCM，不代表它已经被压缩成 AAC。

### 3.4 nb_samples：这块数据每声道有多少采样

这是**一块数据的长度**，不是每秒采样率。

~~~text
sample_rate = 48000
nb_samples = 480

时长 = 480 / 48000 = 0.01 秒 = 10 ms
~~~

双声道时，nb_samples = 480 表示左右各 480 个采样，不是左右加起来 480 个。

项目在 [audio_frame.h](E:/MarkDown/rtsp-cpp/src/media/audio_frame.h) 专门注释了这一点。

<a id="a04"></a>
## 4. 音频帧、采样和字节数有什么关系

### 4.1 音频 AVFrame 是一块采样，不是一张画面

视频帧通常描述一张图像；音频帧描述连续的一块采样。

麦克风这一块可能是 441 个、480 个或其他数量的采样，具体要看读取结果，不能在采集代码里直接认定每块都等于 1024。

本项目的 1024 来自后续 AAC 编码器输入要求，并由项目在打开后校验。

### 4.2 有效字节数公式

~~~text
有效字节数 = nb_samples × channels × bytes_per_sample
时长（秒） = nb_samples / sample_rate
~~~

例如 48000 Hz、双声道、s16、nb_samples = 480：

~~~text
有效字节数 = 480 × 2 × 2 = 1920 字节
时长 = 480 / 48000 = 10 ms
~~~

转换后若为 48000 Hz、单声道、fltp、nb_samples = 480：

~~~text
有效字节数 = 480 × 1 × 4 = 1920 字节
时长仍然是 10 ms
~~~

两个阶段可能恰好字节数相同，但声道、表示方式和采样值已经不同。不要只靠字节数判断音频格式。

以上是有效采样大小，实际分配可能包含对齐空间。AVFrame 的 linesize 也不能直接当作“有效音频总字节数”。

### 4.3 与编码目标的三个不同量

| 名称 | 本项目示例 | 含义 |
|---|---|---|
| sample_rate | 48000 | 每秒、每声道的采样数 |
| frame_size / nb_samples | 1024 | 一次编码输入每声道的采样数 |
| bit_rate | 64000 | 压缩编码目标 bit/s |

1024 个采样在 48000 Hz 下对应约 21.333 ms。由此估算每秒约 46.875 个完整 AAC 采样块，但这不是视频里的画面帧率。


<a id="a05"></a>
## 5. 核心对象与源码地图

### 5.1 项目自己的类型

| 类型 | 主要用途 | 源码 |
|---|---|---|
| MicrophoneConfig | 请求打开哪个设备、希望怎样采集 | [microphone_capture.h](E:/MarkDown/rtsp-cpp/src/media/microphone_capture.h) |
| MicrophoneFormat | 保存打开后的实际采集信息 | 同上 |
| AudioFrame | 包装一块原始 PCM 及其时间、属性 | [audio_frame.h](E:/MarkDown/rtsp-cpp/src/media/audio_frame.h) |
| AudioFrameInput | 原始音频输入接口 | [microphone_audio_source.h](E:/MarkDown/rtsp-cpp/src/media/microphone_audio_source.h) |
| AudioResampler | 封装重采样、格式及声道转换 | [audio_resampler.cpp](E:/MarkDown/rtsp-cpp/src/media/audio_resampler.cpp) |
| AacEncoder | 封装转换、FIFO、组帧、AAC 编码 | [aac_encoder.cpp](E:/MarkDown/rtsp-cpp/src/media/aac_encoder.cpp) |
| AacAccessUnit | 保存一个裸 AAC 访问单元及时间 | [aac_access_unit.h](E:/MarkDown/rtsp-cpp/src/media/aac_access_unit.h) |
| MicrophoneAudioSource | 驱动工作线程、时间线映射与发布 | [microphone_audio_source.cpp](E:/MarkDown/rtsp-cpp/src/media/microphone_audio_source.cpp) |
| AudioFrameQueue | 保存编码后的 AAC 访问单元 | [audio_frame_queue.cpp](E:/MarkDown/rtsp-cpp/src/media/audio_frame_queue.cpp) |

### 5.2 FFmpeg 提供的类型

| 类型 | 初学者可以怎样理解 |
|---|---|
| AVFormatContext | 一个输入的管理对象 |
| AVStream | 输入里的一条音频或视频流 |
| AVCodecParameters | 这条流的媒体参数 |
| AVCodec | 某种编解码器的描述 |
| AVCodecContext | 本次编解码任务的参数和状态 |
| AVPacket | 读取输入或接收编码输出时的一包数据 |
| AVFrame | 原始采样及其内存布局、属性、时间信息 |
| AVChannelLayout | 声道数量、类型与排列 |
| SwrContext | 音频转换配置、滤波与内部缓冲状态 |
| AVAudioFifo | 按采样暂存 PCM 的先进先出缓冲 |
| AVDictionary | 选项名与值组成的字典 |

输入阶段 AVPacket 保存 PCM 设备包，编码输出阶段 AVPacket 保存 AAC。类型相同，不代表里面的数据格式相同。

<a id="a06"></a>
## 6. 服务器怎样建立麦克风输入

源码：[server_main.cpp](E:/MarkDown/rtsp-cpp/src/app/server_main.cpp)。当前服务器枚举麦克风，按索引选择设备：

~~~cpp
const auto microphones = rtsp::MicrophoneCapture::enumerate_devices();
// 实际代码先检查 microphone_index 是否越界。
rtsp::MicrophoneConfig microphone;
microphone.device_name = microphones[microphone_index].name;
~~~

设备枚举由项目调用 Windows COM / DirectShow 接口实现。麦克风枚举使用音频输入设备类别，得到友好名称和用于打开的标识。

随后建立共同时间原点：

~~~cpp
const rtsp::CaptureTimeline timeline;
rtsp::MicrophoneAudioSourceConfig audio_config;
audio_config.timeline = timeline;

auto audio = std::make_unique<rtsp::MicrophoneAudioSource>(
    rtsp::make_microphone_capture_input(microphone),
    audio_config,
    &console_stop);
~~~

当前服务器把同一个时间原点同时交给视频源、音频源与服务器调度。CaptureTimeline 是值类型，复制它保留同一个 T0，不是每复制一次就读取一个新起点。

make_microphone_capture_input() 只创建适配器，尚未真正打开麦克风。见 [microphone_capture_input.cpp](E:/MarkDown/rtsp-cpp/src/media/microphone_capture_input.cpp)：

~~~cpp
if (!opened_) {
    capture_.open(config_, &stopping_);
    opened_ = true;
}
~~~

它在工作线程第一次 read() 时才打开，之后在同一个工作线程读取与关闭。主线程可以通过 request_stop() 请求停止。

<a id="a07"></a>
## 7. 打开麦克风：MicrophoneCapture::open

源码：[microphone_capture.cpp](E:/MarkDown/rtsp-cpp/src/media/microphone_capture.cpp)，搜索 MicrophoneCapture::open。

### 7.1 采集配置与默认值

~~~cpp
struct MicrophoneConfig {
    std::string device_name;
    int sample_rate = 0;
    int channels = 0;
    int audio_buffer_ms = 0;
};
~~~

| 字段 | 意义 | 为 0 时 |
|---|---|---|
| device_name | 要打开的设备标识 | 不能为空 |
| sample_rate | 请求采样率 | 不传相应选项，采用设备默认配置 |
| channels | 请求声道数量 | 不传相应选项 |
| audio_buffer_ms | 请求设备音频缓冲大小，单位 ms | 使用默认缓冲，不是零延迟 |

当前 server_main 主要填写 device_name，其他几个保持默认。因此不能把 AAC 输出固定 48000 Hz 推断为麦克风一定按 48000 Hz 采集。

### 7.2 初始化与选择输入组件

函数先关闭旧输入、校验参数、清空状态并建立停止条件，然后调用：

~~~cpp
avdevice_register_all();
const AVInputFormat* device = av_find_input_format("dshow");
~~~

第一句注册设备组件，第二句查找 DirectShow 输入方式。返回的 device 是组件描述，不是已经打开的麦克风。

### 7.3 只把非零请求传入字典

~~~cpp
AVDictionary* options = nullptr;
if (config.sample_rate) {
    av_dict_set_int(&options, "sample_rate", config.sample_rate, 0);
}
if (config.channels) {
    av_dict_set_int(&options, "channels", config.channels, 0);
}
if (config.audio_buffer_ms) {
    av_dict_set_int(&options, "audio_buffer_size", config.audio_buffer_ms, 0);
}
~~~

实际代码用 require_success() 检查每次设置选项的结果。

这些是请求设备怎样采集，不是把已经采集的数据转换成目标 AAC 格式。DirectShow 的 audio_buffer_size 单位是毫秒；请求缓冲也不等于端到端延迟测量值。[F3]

### 7.4 创建输入上下文并打开设备

~~~cpp
impl_->input = avformat_alloc_context();
impl_->input->flags |= AVFMT_FLAG_NONBLOCK;
impl_->input->interrupt_callback = {Impl::interrupted, impl_.get()};

avformat_open_input(
    &impl_->input,
    ("audio=" + config.device_name).c_str(),
    device,
    &options);
~~~

| 参数 | 含义 | 来源 |
|---|---|---|
| &impl_->input | 可更新的输入上下文指针地址 | 项目持有，FFmpeg 管理输入内容 |
| audio=设备名 | 打开指定音频设备 | 项目拼接，名称来自选择结果 |
| device | 使用 DirectShow | 项目主动指定 |
| &options | 采集请求字典 | 用户配置/默认值 |

avformat_alloc_context() 只分配对象，avformat_open_input() 才打开输入。

非阻塞标志与中断回调配合超时、停止逻辑，不能把它们理解成对任意驱动操作都提供严格硬实时保证。

打开后检查字典是否仍有未消费选项，释放字典。未消费通常提示选项未识别；选项被接收也不替代实际格式校验。

### 7.5 找到音频流并要求 PCM

~~~cpp
// 实际代码遍历 streams，选择 AVMEDIA_TYPE_AUDIO。
const AVStream* stream = impl_->input->streams[impl_->stream_index];
const AVCodecParameters* parameters = stream->codecpar;
const std::string codec_name = avcodec_get_name(parameters->codec_id);

if (codec_name.rfind("pcm_", 0) != 0) {
    throw std::runtime_error("microphone requires PCM input");
}
~~~

codec_id 和 codec_name 来自打开后的输入流，不是我们硬写“它一定是 pcm_s16le”。项目接受的具体 PCM 编码要看设备和 FFmpeg 输入链路实际提供什么。

### 7.6 创建 PCM 解码器

~~~cpp
const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
impl_->decoder = avcodec_alloc_context3(codec);
avcodec_parameters_to_context(impl_->decoder, parameters);
impl_->decoder->pkt_timebase = stream->time_base;
avcodec_open2(impl_->decoder, codec, nullptr);
~~~

按顺序理解：找到适合输入编码的处理器 → 创建任务上下文 → 复制流参数 → 设置输入包时间单位 → 初始化。

PCM 本身未经过 AAC 压缩，但仍可通过 PCM 解码器把设备包处理为带有格式、声道和内存布局的 AVFrame。这一步不是 AAC 解码，也不是后面的重采样。

### 7.7 读取实际信息并校验

~~~cpp
const auto* decoder = impl_->decoder;
const char* sample_format = av_get_sample_fmt_name(decoder->sample_fmt);
// 实际代码检查采样率、声道布局、时间单位有效性。
// 若请求了非零采样率或声道数，还检查协商结果是否相符。
~~~

项目保存：

~~~cpp
impl_->actual = {
    decoder->sample_rate,
    decoder->ch_layout.nb_channels,
    sample_format,
    layout,
    codec_name,
    stream->time_base.num,
    stream->time_base.den
};
~~~

layout 由 av_channel_layout_describe() 将实际布局转换成名称。

| MicrophoneFormat 字段 | 来源 |
|---|---|
| sample_rate | 打开的 PCM 解码上下文 |
| channels | 解码上下文的实际布局 |
| sample_format | sample_fmt 枚举转换为名称 |
| channel_layout | 实际布局转换为描述字符串 |
| codec | 输入流 codec_id 转名称 |
| time_base_num/den | 输入流时间单位 |

MicrophoneConfig 是请求，MicrophoneFormat 是得到的结果。它们不能互相替代。

最后 av_packet_alloc() 和 av_frame_alloc() 建立可复用容器。分配容器不表示已经读到音频。


<a id="a08"></a>
## 8. 读取 PCM：包怎样变成原始音频帧

源码：MicrophoneCapture::read()。

### 8.1 三个核心动作

~~~cpp
av_read_frame(input, packet);             // 从设备读取一个包。
avcodec_send_packet(decoder, packet);    // 把 PCM 包交给解码器。
avcodec_receive_frame(decoder, decoded); // 取得原始音频帧。
~~~

名字里带 frame 的 av_read_frame()，输出参数实际是 AVPacket。不要仅凭名字判断输出类型。

### 8.2 实际循环先取解码输出

简化逻辑：

~~~text
先 receive_frame()
  ├─ 成功：交付 AudioFrame
  ├─ 非 EAGAIN 错误：失败
  └─ EAGAIN：需要更多输入
        ↓
有尚未提交的包吗？
  ├─ 有：send_packet()，成功后释放当前引用，继续取帧
  └─ 无：av_read_frame() 从设备读取
             ├─ 读到目标流包：标记 packet_pending
             ├─ 当前无包：等待或返回超时
             └─ 读取失败：报告错误
~~~

解码器可能保留尚未取出的输出，所以不是每次进入 read() 都马上去设备取新包。编解码接口的输入与输出是分离的，需要按状态推进。[F4]

### 8.3 packet_pending 是项目状态

~~~cpp
bool packet_pending = false;
~~~

成功读到目标流包时设为 true。发送成功后解除包引用，并设回 false。

它表示“当前已经读到一个需要送入解码器的包”。不是 FFmpeg 的设备缓冲数量，也不是已解码帧数。

代码在 receive_frame() 返回 EAGAIN 后发送待处理包；如果 send_packet() 此时也返回 EAGAIN，会报出无法推进的错误，避免无意义忙循环。不要理解为程序会无条件丢掉暂时未接收的包然后继续读取。

### 8.4 两个 EAGAIN 分开看

| 位置 | 意义 | 下一步 |
|---|---|---|
| receive_frame | 没有解码输出，需要更多输入 | 处理待发送包或读新包 |
| av_read_frame | 当前输入暂时没有包 | 检查截止时间，短暂等待 |

源码中单次等待上限还受 5 ms 小步等待与总截止时间约束。零超时也会进行一次非阻塞读取尝试，不是直接跳过所有读取。

### 8.5 返回结果是项目封装

| 返回值 | 含义 |
|---|---|
| FrameReady | 已经拿到有效原始 PCM 帧 |
| Timeout | 本次期限内没有交付音频 |
| Stopped | 收到停止请求 |
| Failed | 读取或解码失败 |

适配器将它们映射成 FrameReady、Waiting、Ended，错误则通过异常报告。单次等待没有数据与永久结束不同。

<a id="a09"></a>
## 9. AudioFrame 各字段来自哪里

源码：MicrophoneCapture::Impl::deliver() 与 [audio_frame.h](E:/MarkDown/rtsp-cpp/src/media/audio_frame.h)。

### 9.1 先检查当前帧的实际格式

deliver() 要求当前帧采样数为正，采样率、采样格式、完整声道布局与打开时的解码格式保持一致。

这意味着当前实现不是中途检测到另一种设备格式就自动无缝切换，而是报告格式改变或帧无效。

### 9.2 保留图像之外的“音频数据引用”

~~~cpp
AVFrame* retained = av_frame_clone(decoded);
frame.buffer = std::shared_ptr<AVFrame>(retained, [](AVFrame* owned) {
    av_frame_free(&owned);
});
~~~

这与视频保活帧的思路相同：建立独立帧对象，通常共享引用计数的采样缓冲区，让采集器能复用 decoded，同时外部上一块音频仍有效。

### 9.3 每个字段的直接来源

| 字段 | 直接来源 | 哪一类信息 |
|---|---|---|
| buffer | clone 得到的 FFmpeg 帧 | 原始采样与布局由输入链路提供，项目管理引用 |
| sequence | sequence++ | 项目生成的编号 |
| sample_rate | actual.sample_rate | 打开后保存的实际采样率 |
| channels | actual.channels | 实际布局中的声道数 |
| sample_format | actual.sample_format | 实际采样格式名称 |
| nb_samples | decoded->nb_samples | 当前块的每声道采样数 |
| source_pts | decoded->pts，缺失时用 best_effort_timestamp | 来源时间信息 |
| time_base | 优先 decoded->time_base，无效时用流单位 | 解释来源时间戳的单位 |
| arrival_steady_us | steady_us() | 程序交付时记录的本机单调时间 |

注意：音频代码优先取 pts，缺失时取 best_effort_timestamp。不要直接把视频那段优先级反过来抄进音频讲解。

### 9.4 三个容易误解的字段

sequence 不是声卡硬件采样计数器，连续编号不能证明此前从未丢样。

arrival_steady_us 不是设备采到第一个采样的精确时刻，也不是系统日历时间。项目在交付时记录它，用于后续估计起点。

source_pts 被保留，但当前音频编码器不逐块根据它重新给编码帧打时间。后面使用采样数量连续计时，具体见第 16、17 节。

最后 av_frame_unref(decoded) 解除采集器当前帧引用，使对象能够复用；外部 buffer 仍持有的音频不因此立即失效。

<a id="a10"></a>
## 10. 音频内存：extended_data、平面与缓冲区

### 10.1 extended_data 是各个平面的地址表

音频的内存布局由格式和声道布局共同决定。

~~~text
双声道 s16：
extended_data[0] → L0 R0 L1 R1 L2 R2 ...

双声道 fltp：
extended_data[0] → L0 L1 L2 ...
extended_data[1] → R0 R1 R2 ...
~~~

extended_data 能统一表达不同数量的音频数据平面。它不是采样值本身，而是通向采样数据的地址表。

不要拿 fltp 的 plane 0 直接当作所有声道交错数据，也不要认为双声道就必然对应两个数据平面。[F2]

### 10.2 项目的 audio_buffer() 做了什么

源码：[audio_ffmpeg.h](E:/MarkDown/rtsp-cpp/src/media/audio_ffmpeg.h)。

~~~cpp
inline std::shared_ptr<AVFrame> audio_buffer(
    int rate, int channels, AVSampleFormat format, int samples) {
    auto frame = std::shared_ptr<AVFrame>(
        av_frame_alloc(),
        [](AVFrame* buffer) { av_frame_free(&buffer); });
    // 实际实现先检查是否分配成功。
    frame->sample_rate = rate;
    frame->format = format;
    frame->nb_samples = samples;
    av_channel_layout_default(&frame->ch_layout, channels);
    check_audio(av_frame_get_buffer(frame.get(), 0), "allocate audio buffer");
    return frame;
}
~~~

逐句理解：

1. av_frame_alloc() 创建帧描述对象。
2. 填写输出采样率、格式、采样容量和布局。
3. av_frame_get_buffer() 分配能容纳这些采样的数据缓冲区。
4. shared_ptr 删除器保证对象销毁时调用 FFmpeg 释放函数。

最后参数 0 表示由 FFmpeg 选择默认对齐，不是分配零字节。

### 10.3 转换时容量与实际采样数可以不同

重采样器可能为 1000 个采样分配空间，但本次实际只输出 960 个。此时缓冲区容量没有变小，nb_samples 要更新为实际有效数量。

不能把尚未填充的剩余容量也写入 FIFO，否则会把无效数据当作声音。

### 10.4 几种复制行为分别判断

- 采集帧 clone 通常增加底层引用，不复制整块 PCM。
- 重采样会产生新的采样缓冲区和采样结果。
- av_audio_fifo_write() 把采样存入 FIFO 自己的缓冲。
- av_audio_fifo_read() 把采样拷入编码输入帧。
- 生成 AacAccessUnit 时，把压缩字节复制进 std::string。

因此这里不是端到端零拷贝流程。先理解所有权和有效数据，再考虑优化。

<a id="a11"></a>
## 11. 先准备 AAC 编码器，确定输出要求

源码：[aac_encoder.cpp](E:/MarkDown/rtsp-cpp/src/media/aac_encoder.cpp)，搜索 AacEncoder::open。

### 11.1 为什么编码器先于实际输入格式建立

工作线程先打开 AAC 编码器，确定“最终需要什么 PCM”；麦克风首次读出数据后，重采样器才知道“输入实际是什么 PCM”。

~~~text
先确定目标格式
    ↓
再拿到实际输入格式
    ↓
根据两端格式创建转换器
~~~

### 11.2 目标码率由项目提供

~~~cpp
struct AacEncoderConfig {
    int bitrate = 64000;
};
~~~

当前实现只把 bitrate 作为这份配置的可变字段，允许范围为 8000～256000。输出采样率、单声道、AAC-LC 等其他规则在实现中固定。

### 11.3 查找原生 AAC 编码器

~~~cpp
const auto* encoder = avcodec_find_encoder_by_name("aac");
impl_->codec = avcodec_alloc_context3(encoder);
impl_->packet = av_packet_alloc();
~~~

"aac" 是项目主动选择的编码器名称。这里不是查询麦克风是否输出 AAC。

### 11.4 查询编码器支持的采样格式

~~~cpp
const void* formats = nullptr;
int count = 0;
avcodec_get_supported_config(
    impl_->codec, encoder,
    AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
    &formats, &count);

codec->sample_fmt = static_cast<const AVSampleFormat*>(formats)[0];
~~~

FFmpeg 报告能力列表，项目选择第一种支持格式。当前代码没有把输出格式字符串写死为 fltp。

学习时常用 fltp 作为原生 AAC 的例子，但真正运行时应以查询结果与 codec->sample_fmt 为准。

### 11.5 明确设置编码规则

~~~cpp
codec->sample_rate = 48000;
codec->time_base = {1, 48000};
codec->bit_rate = config.bitrate;
codec->profile = AV_PROFILE_AAC_LOW;
codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
av_channel_layout_default(&codec->ch_layout, 1);
avcodec_open2(codec, encoder, nullptr);
~~~

| 参数 | 意义 | 来源 |
|---|---|---|
| sample_rate = 48000 | 编码使用 48 kHz PCM | 项目固定 |
| ch_layout 单声道 | 编码使用一个声道 | 项目固定 |
| sample_fmt | 编码器接受的原始格式 | 查询能力后由项目选择 |
| time_base = 1/48000 | 输入编码帧时间的单位 | 项目固定 |
| bit_rate | 压缩目标 bit/s | 项目配置 |
| profile AAC_LOW | 使用 AAC-LC | 项目选择 |
| GLOBAL_HEADER | 请求相应全局配置输出方式 | 项目选择 |

这些赋值只声明编码要求，**不会把麦克风 PCM 自动重采样**。实际转换要看下一节。

### 11.6 打开后读取 frame_size 与配置字节

~~~cpp
if (codec->frame_size != 1024 || codec->sample_rate != 48000 ||
    codec->ch_layout.nb_channels != 1 ||
    !codec->extradata || codec->extradata_size <= 0) {
    throw std::runtime_error(
        "AAC requires 48 kHz mono, 1024 samples and AudioSpecificConfig");
}
~~~

frame_size 是编码器打开后报告的输入组帧要求，项目明确要求它为 1024。不要把“所有音频编码器都用 1024”当成通用规则。

extradata 是编码器提供的配置字节，后续用作 AudioSpecificConfig，不是项目随便拼出的占位数据。

### 11.7 为后续转换和组帧做准备

~~~cpp
impl_->fifo = av_audio_fifo_alloc(codec->sample_fmt, 1, codec->frame_size);
impl_->resampler.open(
    codec->sample_rate,
    av_get_sample_fmt_name(codec->sample_fmt));
~~~

FIFO 接受的是符合编码格式的 PCM。AudioResampler::open() 此刻仅保存目标采样率和格式，真正 SwrContext 的初始化推迟到第一块输入到达。


<a id="a12"></a>
## 12. 重采样、采样格式和声道转换

源码：[audio_resampler.cpp](E:/MarkDown/rtsp-cpp/src/media/audio_resampler.cpp)。

### 12.1 这里一次可能完成三个变化

假设实际采集输入是：

~~~text
44100 Hz、双声道、s16
~~~

目标假设为查询得到的：

~~~text
48000 Hz、单声道、fltp
~~~

这涉及：

1. 44100 → 48000：采样率转换，计算新的采样序列。
2. 双声道 → 单声道：按布局进行声道混合。
3. s16 → fltp：整数表示转浮点，并匹配平面布局。

全部由 libswresample 的同一个转换上下文协调完成；这里仍然是 PCM，并未压缩成 AAC。[F1]

### 12.2 open() 只记录目标，convert() 首次建立上下文

~~~cpp
void AudioResampler::open(int sample_rate, const std::string& sample_format) {
    close();
    impl_->output_format = av_get_sample_fmt(sample_format.c_str());
    // 实际代码检查格式和采样率有效。
    impl_->output_rate = sample_rate;
}
~~~

第一块 AVFrame 到达时，convert() 才能读取真实输入布局、格式和采样率。

~~~cpp
AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_MONO;
swr_alloc_set_opts2(
    &impl_->context,
    &output_layout, impl_->output_format, impl_->output_rate,
    &input.ch_layout,
    static_cast<AVSampleFormat>(input.format), input.sample_rate,
    0, nullptr);
swr_init(impl_->context);
~~~

| 参数组 | 表达什么 | 来源 |
|---|---|---|
| 输出布局、格式、采样率 | 最终需要的 PCM | 项目固定单声道，格式与采样率来自已配置编码器 |
| 输入布局、格式、采样率 | 实际收到的 PCM | 当前输入 AVFrame |
| context | 转换配置、滤波与缓冲状态 | FFmpeg 创建 |

之后 av_channel_layout_copy() 保存输入布局。后续块若突然改变输入格式，当前实现报错，不自动重建转换器继续处理。

### 12.3 每块转换前估计输出容量

~~~cpp
const int estimate = swr_get_out_samples(context, input->nb_samples);
const int capacity = std::max(1, estimate);
auto output = detail::audio_buffer(
    output_rate, 1, output_format, capacity);
~~~

swr_get_out_samples() 给出下一次相应转换可能产生的输出采样数量上界，项目据此分配足够容量。它不是保证“本次恰好输出这么多”。[F1]

### 12.4 真正处理数据

~~~cpp
const int count = swr_convert(
    context,
    output->extended_data, capacity,
    input->extended_data, input->nb_samples);

output->nb_samples = count;
return count == 0 ? nullptr : output;
~~~

| 参数 | 意义 |
|---|---|
| output->extended_data | 结果写到哪些平面 |
| capacity | 每声道最多可以写入多少采样 |
| input->extended_data | 输入采样平面地址 |
| input->nb_samples | 每声道实际输入多少采样 |
| 返回 count | 每声道实际输出多少采样 |

这些数量都是采样数量，不是字节数。负返回值表示错误，实际代码在写入 nb_samples 前检查。

### 12.5 为什么不能假设每块输出都等于简单比例

从时长看，441 个 44.1 kHz 采样对应 10 ms，理想等时长的 48 kHz 序列为 480 个采样。

但一次 swr_convert() 的即时输出可能不等于 480，因为滤波、相位和内部缓冲会产生延迟或跨块状态。要区分：

~~~text
整段音频转换后应有的总时长
与
这一次函数调用立即返回的采样数
~~~

代码使用 count 作为有效输出，最后通过 flush 取出剩余采样。不能每块强行截断或补足为按比例猜出的数量。[F1]

### 12.6 三个常见错误

- 只将 sample_rate 改为 48000，原采样值不变：只是改标签，不是重采样。
- 双声道混成单声道时，nb_samples 直接除以二：声道数改变不意味着时长减半。
- 转换返回 0 就认为没有声音或设备失败：重采样器可能暂时保留数据，项目允许返回空输出。

<a id="a13"></a>
## 13. 为什么转换之后还要 FIFO

### 13.1 两边的分块要求不同

麦克风每次给多少采样取决于采集缓冲与读取；转换器每次输出多少取决于输入和内部状态；AAC 编码器这条链路要求每次提交 1024 个采样。

FIFO 用来连接这些不同的分块节奏：

~~~text
转换块：480 | 480 | 480 | 480 | ...
                      ↓ 先暂存
编码块：1024 | 1024 | ...
~~~

注意上面的 480 是用于演示的假定有效输出量，不是每次重采样的保证。

### 13.2 FIFO 存的是 PCM

创建方式：

~~~cpp
av_audio_fifo_alloc(codec->sample_fmt, 1, codec->frame_size);
~~~

格式与声道数必须与写入采样一致。容量计量单位是每声道采样数，接口不是普通字节队列。[F5]

### 13.3 append() 写入转换结果

~~~cpp
const int size = av_audio_fifo_size(fifo);
av_audio_fifo_realloc(fifo, size + frame->nb_samples);
av_audio_fifo_write(
    fifo,
    reinterpret_cast<void**>(frame->extended_data),
    frame->nb_samples);
stats.resampled_samples += frame->nb_samples;
~~~

实际代码先检查整数溢出、扩容错误，以及实际写入数量等于请求量。

reinterpret_cast<void**>() 是配合 FIFO 接口的指针类型转换，不改变采样值，也不把整数音频变成浮点音频。

### 13.4 用数字跟踪缓存

假设每次转换有效输出 480：

| 步骤 | 写入前剩余 | 本次写入 | 可用总数 | 取出编码 | 最终剩余 |
|---|---:|---:|---:|---:|---:|
| 1 | 0 | 480 | 480 | 0 | 480 |
| 2 | 480 | 480 | 960 | 0 | 960 |
| 3 | 960 | 480 | 1440 | 1024 | 416 |
| 4 | 416 | 480 | 896 | 0 | 896 |
| 5 | 896 | 480 | 1376 | 1024 | 352 |

这解释了为什么一次输入不一定产生一帧 AAC。输入块边界可以消失，采样顺序应保持连续。

### 13.5 不要混淆两个音频缓冲

| 缓冲 | 位于哪里 | 保存什么 | 用途 |
|---|---|---|---|
| AVAudioFifo | AAC 编码前 | 原始 PCM 采样 | 攒够编码输入数量 |
| AudioFrameQueue | AAC 编码后 | AacAccessUnit | 连接生产与消费、限制实时缓存 |

FIFO 不是 AAC 包缓存；编码后队列也不能替代 PCM 组帧。

<a id="a14"></a>
## 14. 按 1024 个采样构造编码输入

源码：AacEncoder::Impl::encode_fifo()。

### 14.1 普通阶段够一帧才取

~~~cpp
while (av_audio_fifo_size(fifo) >= codec->frame_size ||
       (tail && av_audio_fifo_size(fifo) > 0)) {
    // 创建编码帧，取采样，设置时间，提交编码。
}
~~~

tail=false 时，不足 1024 就留给下次；tail=true 时，最后不足一帧也处理。

### 14.2 创建一帧并先清为静音

~~~cpp
auto frame = detail::audio_buffer(
    codec->sample_rate, 1, codec->sample_fmt, codec->frame_size);
av_samples_set_silence(
    frame->extended_data, 0, frame->nb_samples, 1, codec->sample_fmt);
~~~

先按格式清静音，再把有效 PCM 取进来。普通完整帧会覆盖全部有效位置；尾帧不足时，未覆盖的位置继续为静音。

使用 av_samples_set_silence() 而不是假设任意格式的静音都能用同一种字节模式表达。

### 14.3 取出采样并累计统计

~~~cpp
const int count = std::min(av_audio_fifo_size(fifo), codec->frame_size);
av_audio_fifo_read(
    fifo, reinterpret_cast<void**>(frame->extended_data), count);

frame->pts = stats.submitted_samples;
frame->duration = frame->nb_samples;
send(frame.get(), output);

stats.submitted_samples += frame->nb_samples;
stats.tail_padding += frame->nb_samples - count;
~~~

编码输入帧仍声明完整 1024 个采样，其中 count 是真实有效内容，余下可能是尾部静音。

例如最后 FIFO 剩余 352：

~~~text
真实采样 352 + 静音补齐 672 = 编码输入 1024
~~~

补齐没有让真实录音变长，只是在编码表示中增加了尾部填充。后续判断有效时长时需要区分。

### 14.4 submitted_samples 是项目生成的连续计数

~~~text
第一帧输入 PTS：0
第二帧输入 PTS：1024
第三帧输入 PTS：2048
~~~

单位为 1/48000 秒。这个计数不直接使用每块麦克风的来源 PTS。

flush 完整结束后，应有：

~~~text
submitted_samples = resampled_samples + tail_padding
submitted_samples 是 1024 的整数倍
0 <= tail_padding < 1024
~~~

这些关系在 [aac_encoder_tests.cpp](E:/MarkDown/rtsp-cpp/tests/aac_encoder_tests.cpp) 中有断言，但编写本文没有重新运行这些测试，也没有把已有测试代码等同于本次硬件实测。

<a id="a15"></a>
## 15. 提交 PCM，取出 AAC 编码结果

### 15.1 send() 提交原始帧

~~~cpp
int result = avcodec_send_frame(codec, frame);
if (result == AVERROR(EAGAIN)) {
    receive(output);
    result = avcodec_send_frame(codec, frame);
}
receive(output, frame == nullptr);
~~~

实际代码还会在 receive 前检查发送错误。

send_frame() 成功表示编码器接收输入，压缩字节还要由 receive_packet() 取得。若发送暂时不能继续，先取输出，再重试同一帧。[F4]

### 15.2 receive() 循环取包

~~~cpp
const int result = avcodec_receive_packet(codec, packet);
~~~

普通阶段 EAGAIN 表示目前没有更多输出，返回等待后续输入；EOF 表示排空结束。

项目还校验：

- PTS 有效且严格递增。
- packet->size 为正且不超过 8191 字节。
- duration 为正并能放入 uint32_t。

包大小上限与后面的 RTP AU-size 13 位表达能力相配合。8191 是这条实现的约束，不是所有 AAC 数据普遍只有这么大。

### 15.3 复制字节，保留输出包时间

~~~cpp
output.push_back({
    std::string(reinterpret_cast<const char*>(packet->data), packet->size),
    packet->pts,
    static_cast<std::uint32_t>(packet->duration)
});
av_packet_unref(packet);
~~~

AacAccessUnit 定义为：

~~~cpp
struct AacAccessUnit {
    std::string data;             // 裸 AAC，不含 ADTS 头。
    std::int64_t pts = 0;
    std::uint32_t duration = 1024;
};
~~~

虽然 data 使用 std::string，它保存的是二进制，可能含 0 字节，不是可以直接打印的文字。构造时传入长度，所以不会遇到首个 0 字节就停止。

字节复制后有独立所有权，解除 FFmpeg 包引用不影响已生成的访问单元。

### 15.4 encode() 为什么返回集合

~~~cpp
std::vector<AacAccessUnit> AacEncoder::encode(const AudioFrame& input);
~~~

一块采集输入可能不足组帧、也可能够提交多帧；编码器还有内部延迟。因此一次调用可能没有结果，也可能有多个结果。

“读到一块 PCM”不等于“马上得到唯一一个 AAC 包”。


<a id="a16"></a>
## 16. 时间戳、编码延迟与填充

这是音频链路较难但重要的部分。先分清“项目提交给编码器的时间”与“编码器输出包的时间”。

### 16.1 编码输入按采样计数

~~~cpp
codec->time_base = {1, 48000};
frame->pts = stats.submitted_samples;
frame->duration = frame->nb_samples;
~~~

每个时间单位对应 1/48000 秒，1024 对应约 21.333 ms。

输入 PTS 是项目按已经提交的采样数生成的。FIFO 跨越了采集块边界，所以不能直接把当前采集块的 PTS 当作新组出的 1024 块的起点。

### 16.2 输出首包 PTS 为什么可能为负

AAC 编码器可能有初始编码延迟。输入从 0 开始，输出首包就可能从 -initial_padding 开始，以描述对应的预卷或初始采样关系。

例如假设当前编码器报告 initial_padding = 1024：

~~~text
输入帧 PTS：0、1024、2048、...
输出包 PTS：-1024、0、1024、2048、...
~~~

负数不自动代表错误。AVCodecContext 的 initial_padding 表达初始填充/编码延迟相关采样数，具体仍需读取当前编码器结果，不应把示例常量套用到所有编码器。[F6]

项目用 int64_t 保存 AAC PTS，并保留 packet->pts，而不是把每包重新编号成 0、1024、2048。

[aac_encoder_tests.cpp](E:/MarkDown/rtsp-cpp/tests/aac_encoder_tests.cpp) 对输出包时间连续性有检查：

~~~cpp
units[index].pts == -stats.initial_padding
                 + static_cast<std::int64_t>(index) * 1024
~~~

这是本项目当前编码配置下的测试预期，不是所有音频编码都遵守同一个固定帧长的声明。

### 16.3 三类缓冲/延迟要分别理解

| 来源 | 体现 | 怎样处理 |
|---|---|---|
| 重采样内部滤波与缓冲 | 尚未输出的 PCM | flush 重采样器取出有效采样 |
| FIFO 不足编码帧长 | 尾部不足 1024 | 项目补静音，记录 tail_padding |
| AAC 编码器内部状态 | 初始延迟、待输出编码包 | 保留包 PTS，读取 padding 信息，排空编码器 |

把它们统称为“丢了若干采样”会误导排查。数据可能仍在内部缓冲，也可能是为了完整编码帧添加的填充。

### 16.4 统计字段分别来自哪里

| 字段 | 来源与意义 |
|---|---|
| input_samples | 项目累计原始输入每声道采样数 |
| resampled_samples | 项目累计进入 FIFO 的转换后有效采样数 |
| submitted_samples | 项目累计交给编码器的采样，含手动尾部补齐 |
| tail_padding | 项目尾帧手动补的静音采样数 |
| initial_padding | 打开后读取 codec->initial_padding |
| trailing_padding | 排空后读取 codec->trailing_padding |
| skip_samples | 从包 AV_PKT_DATA_SKIP_SAMPLES 附加数据累计读取 |
| discard_padding | 从同一附加数据读取尾部丢弃相关数量 |
| packets | 项目累计输出包数量 |

input_samples 与 resampled_samples 可能使用不同采样率的计数，不能直接相减得出丢样数量。应先按各自采样率换算为时长。

同理，各种 padding 字段可能从不同角度描述相关编码行为，不能未经分析就全部相加。

### 16.5 side data 并不等于已经执行裁剪

~~~cpp
const auto* side = av_packet_get_side_data(
    packet, AV_PKT_DATA_SKIP_SAMPLES, &side_size);
if (side && side_size >= 10) {
    stats.skip_samples += AV_RL32(side);
    stats.discard_padding += AV_RL32(side + 4);
}
~~~

AV_RL32() 读取小端 32 位数。这里是在读取统计信息，不是对音频进行重采样或裁剪。

AacAccessUnit 只保存 data、pts、duration，没有 side data 字段。不能因为代码记录过 skip_samples，就认定所有实时接收端都会自动得到并执行同样的精确头尾裁剪。

### 16.6 有效录音时长与解码总采样数可能不同

例如一段真实转换内容为 R 个采样，项目尾部补 P 个采样后送入编码器。解码后的总输出还可能包括编码器初始延迟相关内容。

~~~text
有效音频 ≠ 任意情况下的裸解码输出总长度
~~~

本项目测试会核对重采样、提交、补齐和解码采样关系，并用脉冲位置检查时间一致性。学习时应分别保留各阶段统计，而不是为让第一包时间等于 0 就覆盖编码器给出的时间。

<a id="a17"></a>
## 17. 共同时间线、线程与编码后队列

### 17.1 首个采样时间怎样估计

源码：MicrophoneAudioSource::produce()。

~~~cpp
if (!start_pts) {
    const auto block_us = rescale_media_pts(
        frame.nb_samples, frame.sample_rate, 1000000);
    const auto first_us = add_media_pts(
        config_.timeline.relative_us(frame.arrival_steady_us),
        -block_us);
    start_pts = rescale_media_pts(
        first_us, 1000000, encoder.track().clock_rate);
}
~~~

逐步解释：

1. nb_samples / sample_rate 算出首块 PCM 的持续时间。
2. 把首块到达相关时刻转为相对共同 T0 的微秒。
3. 减去这块持续时间，估计首个采样的开始时刻。
4. 把微秒换成 48000 Hz 音频时间单位。

例如首块到达时刻距 T0 210 ms，首块包含约 10 ms 音频，则估计首个采样起点在 T0 后 200 ms：

~~~text
start_pts = 0.200 × 48000 = 9600
~~~

这是估计，可能受到设备缓冲、驱动和调度延迟影响，不是硬件级绝对采样时刻校准。

### 17.2 后续采样为什么不逐块用到达时刻打时间

声卡每块到达程序的间隔可能有抖动。若每次按读取时刻重打 PTS，会把调度抖动写进声音时间线。

当前实现只估计一次 start_pts，后面靠编码输入的连续采样数和编码器包时间推进：

~~~text
编码器局部 PTS + start_pts = 共同时间线音频 PTS
~~~

publish() 中：

~~~cpp
frame.pts = add_media_pts(frame.pts, start_pts);
queue_.push(std::move(frame));
~~~

假设 start_pts = 9600、initial_padding = 1024：

~~~text
编码包局部 PTS：-1024、0、1024、...
发布后的 PTS：   8576、9600、10624、...
~~~

仍然保留编码器初始延迟对应的时间关系。

### 17.3 source_pts 被保留，但当前没有逐块用于音频同步

MicrophoneCapture 保留 source_pts，AacEncoder 按采样计数生成编码时间，MicrophoneAudioSource 按首块到达估计起点。

因此不能说“当前每块都直接根据设备 PTS 与视频逐帧对齐”。它也没有在这段代码中根据长期音视频漂移动态改变重采样比例的反馈环。

采集长时间运行、设备时钟不完全一致时的漂移，需要结合更完整的同步策略分析，不能单凭共同 T0 就认为所有时长都能永远无误差对齐。

### 17.4 工作线程怎样串起整条链路

逻辑简化示意：

~~~cpp
AacEncoder encoder;
encoder.open(config_.encoder);

while (!stopping_.load()) {
    AudioFrame frame;
    auto result = input_->read(frame, config_.read_timeout);
    if (stopping_.load()) break;
    if (result == AudioInputResult::Waiting) {
        // 实际实现检查持续无 PCM 是否超过 no_data_timeout。
        continue;
    }
    if (result == AudioInputResult::Ended) {
        publish(encoder.flush(), encoder, start_pts.value_or(0));
        break;
    }
    // 首块估计 start_pts，后续继续沿用。
    publish(encoder.encode(frame), encoder, *start_pts);
}
~~~

构造函数启动工作线程后，会等待首个成功入队的 AAC 和轨道信息发布。超时、停止、生产者失败会触发相应退出逻辑。

### 17.5 AudioFrameQueue 的容量与丢弃规则

MicrophoneAudioSourceConfig 中：

~~~cpp
AudioFrameQueueLimits queue{9};
~~~

9 是这份配置的最大帧数；AudioFrameQueueLimits 类型自身的默认帧数是 32。聚合初始化只覆盖第一个字段，字节上限仍用类型默认值 128 KiB。

9 个完整 1024 采样 AU 在 48 kHz 下约为：

~~~text
9 × 1024 / 48000 = 0.192 秒
~~~

这是按帧数估算的队列容量，不是系统总延迟；字节限制、消费速度也会影响实际缓存。

队列满时，当前 push() 丢弃较老的 AU 给新 AU 腾空间。PCM FIFO 负责组帧，编码后队列负责实时缓存，两者完全不同。

### 17.6 Waiting 与 Ended 要分清

AudioFrameQueue::read() 在队列为空但未关闭时返回 Waiting；关闭且已取空时返回 Ended；有排队帧时先交付。

MicrophoneAudioSource 的 ended_ 在消费者确认队列终态后更新，避免生产者刚完成就让消费者遗漏最后几个包。

<a id="a18"></a>
## 18. 结束、排空、停止与释放

### 18.1 flush 顺序很重要

AacEncoder::flush() 的真实顺序是：

~~~cpp
while (auto frame = impl_->resampler.flush()) {
    impl_->append(frame);
}
impl_->encode_fifo(output, true);
impl_->send(nullptr, output);
~~~

对应三步：

1. **排空重采样器**：把内部保留的有效 PCM 取出，继续写 FIFO。
2. **处理 FIFO 尾部**：按完整 1024 采样提交，最后不足时补静音。
3. **排空 AAC 编码器**：发送空帧，取出尚未输出的编码结果。

如果只清空 FIFO 而不排空重采样器，可能遗漏有效尾部采样；如果只发送空帧而未提交 FIFO 尾部，编码器根本没有收到那部分 PCM。

### 18.2 重采样器的空输入

AudioResampler::flush() 内部通过空输入调用转换：

~~~cpp
swr_convert(context,
    output->extended_data, capacity,
    nullptr, 0);
~~~

这是通知没有新的输入，取出内部剩余内容，不是把输出改为“零采样率”。需要重复取，直到没有更多输出。[F1]

### 18.3 编码器的空帧

~~~cpp
avcodec_send_frame(codec, nullptr);
~~~

空帧表示输入结束，进入 draining，不是编码一帧静音。静音尾部是前面创建完整 AVFrame 并清静音后提交的另一件事。

普通 receive() 遇 EAGAIN 可返回；排空阶段项目期待继续接收到 EOF，否则按错误处理。[F4]

### 18.4 不是每一种退出都保证完整排空

自然输入结束分支调用 encoder.flush()。显式停止时，produce() 会在停止检查后退出，stop() 回收工作线程并清除队列。

close() 或析构主要释放资源，不隐式保证完整输出尾部 AAC。因此“停止实时服务”和“完整保存一段录音文件”不能直接视为相同的结束需求。

### 18.5 资源释放速查

| 对象或数据 | 对应操作 |
|---|---|
| 当前输入包数据引用 | av_packet_unref() |
| 当前解码帧数据引用 | av_frame_unref() |
| AVPacket 对象 | av_packet_free() |
| AVFrame 对象 | av_frame_free() |
| 输入/编码上下文 | avcodec_free_context() |
| 麦克风输入 | avformat_close_input() |
| 重采样器 | swr_free() |
| 声道布局附带资源 | av_channel_layout_uninit() |
| PCM FIFO | av_audio_fifo_free() |
| 参数字典 | av_dict_free() |

解除一个引用不等于销毁所有仍被其他对象使用的底层缓冲。close() 与 request_stop() 也不同：后者先发出停止条件，让执行读取的线程协调退出。


<a id="a19"></a>
## 19. 跟踪一段音频的完整示例

下面分成两个例子：第一个看重采样后的整体关系，第二个看已经得到有效 PCM 后如何组帧。

### 19.1 示例一：10 ms 双声道 PCM 进入转换器

假设设备打开后的真实输入为：

~~~text
sample_rate = 44100
channels = 2
sample_format = s16
nb_samples = 441
~~~

这块时长与有效大小：

~~~text
441 / 44100 = 10 ms
441 × 2 × 2 = 1764 字节
~~~

这些格式和采样数量要从输入帧读，不能因为例子如此，就在生产代码里写死它们。

AAC 目标是项目配置的 48000 Hz、单声道，假设查询的输入采样格式为 fltp。swr_convert() 会完成重采样、混音和采样表示转换。

从时长看，10 ms 对应目标 480 个采样；从单次调用看，实际返回 count 可能因转换器内部状态不同而不同。程序必须使用 count。

~~~text
转换结果 → 追加 FIFO → 够 1024 才编码
~~~

这块 10 ms 数据本身通常不足一次完整 AAC 编码输入，所以 encode() 返回空集合并不一定是失败。

### 19.2 示例二：假设已经转换得到连续五块 480 采样

这是为了展示组帧的假设，不是在预测每次 swr_convert() 的返回值。

~~~text
五块有效采样合计：5 × 480 = 2400
可以提交两帧：2 × 1024 = 2048
FIFO 剩余：2400 - 2048 = 352
~~~

两帧输入的局部 PTS：

~~~text
第 1 帧：0
第 2 帧：1024
~~~

若此时自然结束，且重采样器已无额外内容：

~~~text
尾帧：352 个真实采样 + 672 个静音采样
提交后总采样：3072
有效转换后采样：2400
tail_padding：672
~~~

不含编码器初始延迟的有效时长：

~~~text
2400 / 48000 = 50 ms
~~~

提交表示长度：

~~~text
3072 / 48000 = 64 ms
~~~

差出的 14 ms 是该示例的手动尾部补齐，不是新采集到的真实声音。

### 19.3 示例三：共同时间与编码器延迟一起看

假设：

~~~text
第一块到达相关时刻：T0 + 210 ms
首块时长：10 ms
估计首采样起点：T0 + 200 ms
start_pts：9600
编码器报告 initial_padding：1024
~~~

那么按该示例，局部首包 PTS 可能是 -1024，发布后成为：

~~~text
-1024 + 9600 = 8576
8576 / 48000 ≈ 178.667 ms
~~~

这个提前约 21.333 ms 的编码包时间，反映编码器初始延迟关系。不要为让首包等于估计起点 200 ms 就随意改写包时间。

真正有效声音的位置还要结合编码延迟与解码行为分析，不能单看第一包时间。

<a id="a20"></a>
## 20. AAC 轨道描述与 RTP 衔接

这一节只讲音频处理怎样交给项目后续模块，不展开完整 RTSP 协商。

### 20.1 配置字节从编码器读取

AacEncoder::open() 把 codec->extradata 转成十六进制字符串 config_hex，然后建立轨道：

~~~cpp
impl_->track = {
    "audio", "trackID=1", 97, "MPEG4-GENERIC", 48000, 1,
    "streamtype=5;profile-level-id=1;mode=AAC-hbr;config=" + config_hex +
    ";SizeLength=13;IndexLength=3;IndexDeltaLength=3"
};
~~~

| 内容 | 来源 |
|---|---|
| audio、trackID=1 | 项目约定 |
| 动态负载类型 97 | 项目选择，后续按轨道描述解释 |
| MPEG4-GENERIC、AAC-hbr 等 | 项目选择的传输描述 |
| 48000、1 声道 | 与本项目固定编码输出对应 |
| config= 后的字节 | 编码器 extradata，经项目转成十六进制 |

十六进制转换只改变配置字节的文字表达，不是再编码一次音频。AudioSpecificConfig 描述解码配置，不等于完整音频内容。

### 20.2 裸 AAC、ADTS 和 RTP 负载不同

~~~text
AacAccessUnit.data：当前实现的裸 AAC 数据
ADTS：某些 AAC 文件/流使用的头部封装方式
RTP：项目用于网络传输的打包格式
~~~

当前 AacAccessUnit 不包含 ADTS 头。不要直接认为把 data 写到文件后，任何工具都能像读取带 ADTS 的 .aac 一样自动识别全部配置。

### 20.3 项目添加 AU 头，不再调用 AAC 编码器

源码：[aac_packetizer.cpp](E:/MarkDown/rtsp-cpp/src/media/aac_packetizer.cpp)。

~~~cpp
std::string payload("\0\x10", 2);
payload += static_cast<char>(size >> 5);
payload += static_cast<char>((size & 31) << 3);
payload += frame.data;
~~~

实际源码中字符串使用 C++ 转义，前两个字节为 00 10，表示 AU-headers-length 的 16 位长度；后面按项目约定写入 13 位 AU-size 和 3 位 AU-index，再追加 AAC 字节。

这是项目自己的传输封装。AAC 编码在此前已经完成。

RtpPayloadUnit 同时保留 frame.pts，并设 clock_rate = sample_rate。后续 RTP 发送层再把媒体时间映射到该轨的 RTP 时间戳，不能把一个 RTP 随机起点直接当作文件中的采样序号。

<a id="a21"></a>
## 21. 函数速查表与信息来源总表

### 21.1 设备输入与原始帧

| 函数 | 做什么 | 主要参数来源 |
|---|---|---|
| avdevice_register_all | 注册设备组件 | 项目调用 |
| av_find_input_format | 查找 DirectShow 输入 | 项目指定 dshow |
| av_dict_set_int | 设置采集选项 | 用户配置或默认值 |
| avformat_alloc_context | 创建输入管理对象 | 项目发起 |
| avformat_open_input | 打开麦克风、建立流信息 | 设备名、输入组件、采集请求 |
| avcodec_find_decoder | 找到 PCM 解码器 | 实际输入 codec_id |
| avcodec_parameters_to_context | 复制输入参数 | 实际流参数 |
| avcodec_open2 | 初始化编解码任务 | 准备好的上下文 |
| av_read_frame | 读取输入包 | 已打开设备 |
| avcodec_send_packet | 提交包给解码器 | 设备输入包 |
| avcodec_receive_frame | 取出 PCM 原始帧 | 解码器状态 |
| av_frame_clone | 保留帧及其数据引用 | 当前解码帧 |
| av_get_sample_fmt_name | 格式枚举转名称 | 实际格式或目标格式 |
| av_channel_layout_describe | 布局转描述 | 实际布局 |

### 21.2 转换、组帧和编码

| 函数 | 做什么 | 注意点 |
|---|---|---|
| avcodec_get_supported_config | 查询编码器能力 | 项目选择第一种支持格式 |
| av_channel_layout_default | 构建默认布局 | 输出指定 1 声道 |
| swr_alloc_set_opts2 | 设置转换两端格式 | 输入来自实际帧，输出来自编码要求 |
| swr_init | 初始化转换器 | 此时还未输入这块 PCM |
| swr_get_out_samples | 估计下一次输出采样上界 | 是容量估计，不是实际 count |
| swr_convert | 计算转换后 PCM | 返回每声道实际采样数量 |
| av_frame_get_buffer | 分配采样缓冲 | 先填格式、布局、nb_samples |
| av_audio_fifo_alloc | 创建 PCM FIFO | 按采样而非按包计量 |
| av_audio_fifo_size | 查询已有采样数 | 每声道数量 |
| av_audio_fifo_realloc | 扩容 FIFO | 不会编码音频 |
| av_audio_fifo_write | 保存 PCM | 不转换采样格式 |
| av_audio_fifo_read | 取 PCM 构造编码帧 | 通常取 1024 |
| av_samples_set_silence | 按格式清静音 | 用于尾部补齐 |
| avcodec_send_frame | 提交 PCM 编码帧 | 空帧表示排空 |
| avcodec_receive_packet | 取得压缩 AAC | 保留包的 PTS/duration |
| av_packet_get_side_data | 读取附加信息 | 这里用于 skip/padding 统计 |
| av_packet_unref | 解除包当前引用 | 已复制的访问单元仍有效 |

### 21.3 逐项区分信息来源

| 信息 | 谁决定或提供 |
|---|---|
| 设备名称 | 用户选择，标识可由设备枚举取得 |
| 请求采样率、声道数、缓冲 | MicrophoneConfig，0 时不传对应选项 |
| 实际 PCM 编码、采样率、格式、布局 | 输入链路与打开后的解码上下文 |
| 当前块 nb_samples | 解码帧提供 |
| 来源 PTS/time_base | FFmpeg 输出帧或流提供 |
| 到达相关时间 | 项目读取本机时钟 |
| sequence | 项目编号 |
| AAC 编码器名称 | 项目选择 aac |
| AAC 48 kHz、单声道、LC | 项目固定策略 |
| AAC 输入 sample_fmt | FFmpeg 报告支持列表，项目选第一种 |
| 编码 frame_size | 打开后编码器报告，项目要求等于 1024 |
| 转换后采样与返回 count | swresample 计算 |
| FIFO 如何组块、尾部补齐 | 项目决定，FFmpeg FIFO 执行存取 |
| 输入编码 PTS | 项目累计已提交采样 |
| 输出包 PTS、duration、大小 | 编码器提供，项目校验保留 |
| 初始/尾部编码 padding | 编码器与包附加信息提供 |
| 手动 tail_padding | 项目尾部组帧时累计 |
| start_pts | 项目按首块到达时刻与块时长估计 |
| 入队 PTS | 编码包局部 PTS 加 start_pts |
| AAC 配置 extradata | 编码器提供 |
| 轨道号、PT、传输格式描述 | 项目约定 |
| AAC 编码字节 | 编码器产生，不是麦克风直接输出 |


<a id="a22"></a>
## 22. 视频对照、常见误解与学习练习

### 22.1 和视频链路对照

| 阶段 | 视频 | 音频 |
|---|---|---|
| 采集设备 | 摄像头 | 麦克风 |
| 原始数据 | 像素 | PCM 采样 |
| 原始帧重点 | 宽高、像素格式、行跨度 | 采样率、声道布局、采样格式、nb_samples |
| 转换器 | SwsContext | SwrContext |
| 处理调用 | sws_scale | swr_convert |
| 转换目标 | 本项目 YUV420P | 本项目 48 kHz、单声道、编码器支持格式 |
| 输入分块衔接 | 一张图像转换后提交 | 采样进入 FIFO 后按 1024 重新组帧 |
| 编码器 | libx264 | FFmpeg 原生 aac |
| 编码接口 | send_frame / receive_packet | 同样的接口 |
| 输出 | H264AccessUnit，一组 NALU | AacAccessUnit，裸 AAC 字节 |
| 媒体时钟 | 本项目 90 kHz | 本项目 48 kHz |
| 启动等待 | 可用 IDR、SPS/PPS | 首个成功入队的 AAC 与轨道信息 |

两条链路共享“先有原始数据，再转换，再编码”的思想，但音频的采样数量与重新组帧尤其重要。

### 22.2 常见误解

**麦克风输出一定已经是 48000 Hz、单声道？**

不一定。采集配置默认不指定这些值；编码目标才固定为这个组合，中间由转换器衔接。

**PCM 就等于 s16？**

不是。整数、浮点，packed、planar，以及不同采样率和布局，都可能用于原始 PCM。

**采样率越大，音量就越大？**

采样率描述时间上的采样密度。音量主要与采样信号幅度及后续处理有关，不能把它们等同。

**nb_samples = 1024 表示 1024 字节？**

表示每声道 1024 个采样，字节数还要乘声道数和单采样字节数。

**双声道混单声道，时长就变一半？**

不会因为声道数减少而自动改变时长。转换改变声道内容，时间长度仍由采样数与采样率决定。

**把 sample_rate 字段改掉就完成重采样？**

不会。需要实际计算采样并按正确采样率解释结果。

**FIFO 会把采样压缩成 AAC？**

不会。FIFO 暂存 PCM，压缩由编码器完成。

**每次读取都应产生一个 AAC 包？**

读取块、转换块、编码输入块与编码输出包的边界不同，还存在编码延迟。

**首包 PTS 为负必须改为 0？**

不能这样笼统处理。当前实现保留编码器包时间，以维持初始延迟与后续采样对应关系。

**记录了 skip_samples，播放器就一定能裁掉它们？**

当前访问单元没有保存完整 side data，后续实时传输也不能仅凭本地统计自动完成精确裁剪。

**共同时间原点能解决所有音视频同步问题？**

它有助于对齐起点与媒体调度，但不是硬件级精确测时，也不是长期设备时钟漂移补偿环。

**队列最大 9 帧，整个系统延迟就一定是 192 ms？**

192 ms 是该 AU 数量的采样时长估算，还存在采集缓冲、组帧、编码、网络和接收播放等延迟。

**关闭对象就会保存全部尾部音频？**

完整保留尾部要走相应排空流程；显式停止实时服务时，当前代码会清除待消费队列。

### 22.3 建议按三遍阅读

第一遍只看数据方向：

~~~text
MicrophoneConfig
  → AVFormatContext / AVStream
  → AVPacket
  → PCM AVFrame / AudioFrame
  → 转换后的 PCM
  → AVAudioFifo
  → 1024 采样的 AVFrame
  → AAC AVPacket
  → AacAccessUnit
  → AudioFrameQueue
~~~

第二遍在每个赋值旁标注：请求值、实际输入信息、算法结果、项目计数或时间映射。

第三遍深入 EAGAIN、分平面、缓冲引用、编码延迟与排空。

### 22.4 源码阅读顺序

用文件与函数名定位，避免代码调整后固定行号失效。

| 顺序 | 文件 | 搜索符号 | 主要问题 |
|---|---|---|---|
| 1 | [server_main.cpp](E:/MarkDown/rtsp-cpp/src/app/server_main.cpp) | MicrophoneConfig、audio_config | 输入怎样选择，时间原点怎样共享 |
| 2 | [microphone_capture.h](E:/MarkDown/rtsp-cpp/src/media/microphone_capture.h) | MicrophoneConfig、MicrophoneFormat | 请求与实际结果 |
| 3 | [microphone_capture_input.cpp](E:/MarkDown/rtsp-cpp/src/media/microphone_capture_input.cpp) | if (!opened_) | 何时真正打开设备 |
| 4 | [microphone_capture.cpp](E:/MarkDown/rtsp-cpp/src/media/microphone_capture.cpp) | MicrophoneCapture::open | 输入、流参数、PCM 解码器 |
| 5 | 同上 | MicrophoneCapture::read | 设备包到原始帧 |
| 6 | 同上 | deliver(AudioFrame | 字段来源与保活 |
| 7 | [aac_encoder.cpp](E:/MarkDown/rtsp-cpp/src/media/aac_encoder.cpp) | AacEncoder::open | 目标 PCM 与编码要求 |
| 8 | [audio_ffmpeg.h](E:/MarkDown/rtsp-cpp/src/media/audio_ffmpeg.h) | audio_buffer | 分配采样缓冲 |
| 9 | [audio_resampler.cpp](E:/MarkDown/rtsp-cpp/src/media/audio_resampler.cpp) | swr_alloc_set_opts2、swr_convert | 实际输入与目标输出怎样衔接 |
| 10 | [aac_encoder.cpp](E:/MarkDown/rtsp-cpp/src/media/aac_encoder.cpp) | append、encode_fifo | 缓存与重新组帧 |
| 11 | 同上 | send、receive | 编码接口和输出时间 |
| 12 | 同上 | AacEncoder::flush | 三步排空 |
| 13 | [microphone_audio_source.cpp](E:/MarkDown/rtsp-cpp/src/media/microphone_audio_source.cpp) | produce、publish | 起点估计、时间映射和工作线程 |
| 14 | [audio_frame_queue.cpp](E:/MarkDown/rtsp-cpp/src/media/audio_frame_queue.cpp) | push、read、finish | 编码缓存、丢弃与终态 |
| 15 | [aac_packetizer.cpp](E:/MarkDown/rtsp-cpp/src/media/aac_packetizer.cpp) | packetize | 编码字节怎样接入 RTP |

### 22.5 复习练习

1. 44100 Hz、双声道、s16，nb_samples=441，时长和有效字节数各是多少？
2. 同一块数据转为 48000 Hz 后，从时长推算大约对应多少采样？为什么本次 swr_convert 的返回数可能不同？
3. 双声道 fltp 有几个平面？s16 交错格式又有几个？
4. MicrophoneConfig.sample_rate=0 与 codec->sample_rate=48000 是否矛盾？
5. sample_fmt 是写死的，还是通过编码器能力查询选择的？
6. 哪个地方读取 actual 格式，哪个地方给新的编码输入分配内存？
7. FIFO 有 2400 个有效采样时，普通阶段提交几帧、剩余多少？
8. 若此时输入结束且重采样器已排空，尾部应补多少个静音采样？
9. 为什么把采集块 PTS 原样赋给 FIFO 组出的所有编码帧不合适？
10. 输入 PTS 从 0 开始，编码包首个 PTS 为什么可能小于 0？
11. publish() 加的 start_pts 怎样估计？每块都重新估计吗？
12. source_pts 当前是否逐块用于改变重采样比例？
13. 为什么自然输入结束时必须先排空重采样器，再处理 FIFO，再排空编码器？
14. AVAudioFifo 与 AudioFrameQueue 各自保存什么？
15. data 使用 std::string 为什么仍然可以保存二进制 AAC？
16. extradata、ADTS 头和 RTP AU 头是不是同一种内容？

推荐把答案写成“输入是什么 → 函数做什么 → 输出是什么 → 参数来自哪里”的四句话。能这样解释，才算把函数放回了数据链路。

## 参考与核对范围

本项目的行为结论以文中链接的当前源码为依据；测试文件用于说明已有断言与验证思路，本文编写过程没有打开硬件、重新运行编码测试或测量实时延迟。

以下官方文档用于继续查阅通用 FFmpeg API。函数可用性和字段细节应与本项目实际链接的 FFmpeg 开发包版本匹配，不要把网上其他版本的结构体字段直接照搬。

| 标记 | 官方参考 | 对应知识 |
|---|---|---|
| F1 | FFmpeg libswresample 文档，地址见下方 | 转换参数、输出上界、实际输出、排空 |
| F2 | FFmpeg sample formats 文档，地址见下方 | packed/planar、采样表示与字节数 |
| F3 | FFmpeg devices 文档，地址见下方 | DirectShow 输入及缓冲选项 |
| F4 | FFmpeg send/receive API 文档，地址见下方 | 编解码状态、EAGAIN、draining |
| F5 | FFmpeg audio FIFO 文档，地址见下方 | 按每声道采样数暂存 PCM |
| F6 | FFmpeg AVCodecContext 文档，地址见下方 | initial_padding、frame_size 等字段 |

~~~text
F1: https://ffmpeg.org/doxygen/trunk/group__lswr.html
F2: https://ffmpeg.org/doxygen/trunk/group__lavu__sampfmts.html
F3: https://ffmpeg.org/ffmpeg-devices.html
F4: https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html
F5: https://ffmpeg.org/doxygen/trunk/group__lavu__audiofifo.html
F6: https://ffmpeg.org/doxygen/trunk/structAVCodecContext.html
~~~

后续源码有调整时，优先重新核对这几处：服务器配置、AAC 固定输出约束、首块时间起点算法、FIFO 尾部策略、显式停止是否排空、编码后队列容量与丢弃行为。
