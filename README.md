# C++ RTSP 客户端与服务器

这是一个面向新手的协议学习项目：自己实现 RTSP 会话控制、RTP 收发，以及 H.264 / AAC 的封包和解包。

当前已完成实施路线的 **第 1～5 步：工程骨架、TCP 收发、RTSP 消息拆分、SDP 与完整控制流程，以及内存中的 RTP / H.264 封包解包**。`rtsp_client` 与 `rtsp_server` 可以顺序完成 OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN。第五步的媒体模块通过内存测试验证，两个程序仍在 PLAY 成功后立即 TEARDOWN，第六步再接入文件和实际媒体收发。

## 当前实现与运行

已实现的代码：

| 文件 | 作用 |
| --- | --- |
| `CMakeLists.txt` | 构建共用库、客户端、服务器和测试 |
| `src/net/socket_utils.h/.cpp` | Winsock 初始化、清理和错误报告 |
| `src/net/tcp_socket.h/.cpp` | 创建连接、监听、接受客户端、收发和关闭 |
| `src/app/command_line.h` | 检查命令行端口参数 |
| `src/rtsp/rtsp_message.h/.cpp` | 保存、解析和生成 RTSP 请求/响应 |
| `src/rtsp/rtsp_stream_parser.h/.cpp` | 累积 TCP 字节，拆出文本消息或交织帧 |
| `src/rtsp/rtsp_session_utils.h/.cpp` | TCP Transport、Session 字段和控制 URL |
| `src/sdp/session_description.h/.cpp` | 单路 H.264 视频的 SDP 生成与解析 |
| `src/rtp/rtp_packet.h/.cpp` | RTP 公共头、CSRC、扩展头、Padding 的解析与生成 |
| `src/media/h264_packetizer.h/.cpp` | 原始 NALU 转换为单 NALU 或 FU-A 负载 |
| `src/media/h264_depacketizer.h/.cpp` | 单 NALU、FU-A、STAP-A 解包与连续性检查 |
| `src/rtsp/rtsp_server_session.h/.cpp` | 五种请求、状态切换和错误响应 |
| `src/rtsp/rtsp_client.h/.cpp` | 客户端顺序请求与响应校验 |
| `src/app/client_main.cpp` | 建立连接，运行完整控制流程 |
| `src/app/server_main.cpp` | 接待一个客户端，拆消息并调用服务器会话 |
| `tests/tcp_tests.cpp` | 顺序验证二进制收发、半关闭、资源释放和参数检查 |
| `tests/rtsp_tests.cpp` | 验证文本消息、消息体、任意半包、粘包和交织帧 |
| `tests/session_tests.cpp` | 验证 SDP、URL、通道协商、会话状态与错误请求 |
| `tests/rtp_tests.cpp` | 验证 RTP 固定字节、可选头、截断和字段上限 |
| `tests/h264_tests.cpp` | 验证 H.264 固定负载、重组恢复和完整内存链路 |

构建与测试：

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

使用 Visual Studio 生成器时，在两个终端依次运行：

```powershell
# 终端一：先启动服务器，等待一个客户端。
.\build\Debug\rtsp_server.exe 8554 127.0.0.1

# 终端二：运行五种请求，TEARDOWN 成功后客户端和服务器均退出。
.\build\Debug\rtsp_client.exe 127.0.0.1 8554
```

使用 MinGW 或 Ninja 等单配置生成器时，可执行文件通常位于 `build` 目录，去掉上述路径中的 `Debug\`。两个程序均支持 `--help`；不传参数时使用 `127.0.0.1:8554`。服务器接待一次连接后退出，再次测试时需重新启动。客户端不再接受第三个消息参数。

客户端预期输出：

```text
Connected to 127.0.0.1:8554
> OPTIONS rtsp://127.0.0.1:8554/live, CSeq=1
< 200 OK
Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN
> DESCRIBE rtsp://127.0.0.1:8554/live, CSeq=2
< 200 OK
> SETUP rtsp://127.0.0.1:8554/live/trackID=0, CSeq=3
< 200 OK
Video: H264/90000, payload=96
Track: rtsp://127.0.0.1:8554/live/trackID=0
Session: 12345
Channels: 0/1
> PLAY rtsp://127.0.0.1:8554/live/, CSeq=4
< 200 OK
PLAY succeeded (control flow only; no media in this step).
> TEARDOWN rtsp://127.0.0.1:8554/live/, CSeq=5
< 200 OK
RTSP session completed.
```

上例 Session 仅作示意，实际由服务器生成。服务路径固定为 `/live`，视频轨道为 `/live/trackID=0`。SETUP 前收到交织帧，或收到未协商通道的帧，会报错并关闭连接；程序入口目前仍跳过已协商通道的帧，第六步再接入已经实现的 RTP / H.264 模块。RTCP 仍只分流。

本阶段只需要 C++17 编译器、CMake 和 Windows Socket，不依赖 FFmpeg。测试使用本机回环连接、系统自动分配的端口，以及包含零字节的约 2 MiB 数据。客户端和服务器均只支持数字 IPv4 地址，例如 `127.0.0.1`，暂不解析 `localhost` 等主机名。

### 从这里阅读代码

教学版只保留一个 `TcpSocket` 类，先创建对象，再依次调用普通成员函数，不涉及移动语义、友元、智能指针或返回 Socket 对象的静态工厂。

服务器的主流程：

```cpp
rtsp::net::TcpSocket listener;        // 一个空对象
listener.listen("127.0.0.1", 8554);  // 内部依次调用 socket、bind、listen

rtsp::net::TcpSocket connection;      // 另一个空对象
listener.accept_client(connection);  // 新连接保存到 connection，监听对象保持不变

// 在 connection 上循环接收，交给 RtspStreamParser 拆消息后回复。
connection.close();
listener.close();
```

客户端的主流程：

```cpp
rtsp::net::TcpSocket connection;
connection.connect("127.0.0.1", 8554);
rtsp::RtspMessage request;
request.method = "OPTIONS";
request.uri = "rtsp://127.0.0.1:8554/live";
request.set_header("CSeq", "1");
std::string wire = rtsp::serialize_rtsp_message(request);
connection.send_all(wire.data(), wire.size());

// 实际五步流程已封装在 RtspClient::run 中；每次响应都通过流解析器读取。
connection.close();
```

建议依次阅读 `rtsp_message.h`（数据结构）、`rtsp_message.cpp`（文本格式）、`rtsp_stream_parser.cpp`（缓冲拆分），最后看两个入口如何把收发与解析连接起来。实现使用结构体、字符串、顺序循环和普通函数。

第四步建议先读 `session_description.h/.cpp`（SDP 字段），再读 `rtsp_session_utils.cpp`（Transport 和 URL），然后读 `RtspServerSession::handle()`（状态机）及 `RtspClient::run()`（五次请求），最后看两个程序入口。没有新增模板、回调、智能指针或线程框架。

### 第三步：如何判断消息完整

RTSP 文本消息先找 `\r\n\r\n`，得到消息头结束位置，再按 `Content-Length` 等待足够的消息体。没有该头字段时，消息体长度为 0。消息体按字节计算，不能按字符数、空行或者连接关闭判断结束。

交织帧只在当前消息起点检查 `$`，再读取通道号和两个长度字节。长度为 `高字节 * 256 + 低字节`，不包含前面的 4 字节；负载中的 `$`、零字节或空行均作为普通数据保存。

每次收到 TCP 数据后，按下面的固定流程处理：

```cpp
rtsp::RtspStreamParser parser;  // 放在接收循环外，保留上次没收完的数据。
rtsp::RtspStreamItem item;

// 放在接收循环内，count 是 receive 的实际返回值。
parser.append(buffer, count);
while (parser.next(item)) {
    if (item.is_interleaved) {
        // 使用 item.channel 和 item.payload。
    } else {
        // 使用 item.message，按 is_request 区分请求和响应。
    }
}
// next 返回 false 表示字节还不够，继续 receive，不能当成解析失败。
// 对端关闭后先确认所有完整项都已取出，再调用 parser.finish() 检查截断。
```

接口约定：

- `RtspMessage` 共用一个简单结构体：请求用 `method`、`uri`，响应用 `status_code`、`reason`；两者都有 `headers` 和 `body`。
- `header()` 和 `set_header()` 不区分头字段名大小写。每条消息要求一个数字 `CSeq`，本教学版限制范围为 `0..2147483647`；拒绝重复 `CSeq` 和重复 `Content-Length`。
- `parse_rtsp_message()` 一次解析开头的一条消息，`consumed` 是已消费字节数；不完整时返回 `false` 且不修改输出，格式错误时抛出异常。
- `serialize_rtsp_message()` 自动重新计算 `Content-Length`；`body` 和交织帧负载都使用 `std::string` 按字节保存，可包含零字节。
- 当前仅接受 RTSP 1.0、CRLF 换行和单行头字段，不支持旧式折行头字段。解析错误后由入口关闭当前连接，不尝试从错误字节中猜测下一条消息的起点。
- 上限为消息头 16 KiB、消息体 1 MiB、交织帧负载 65535 字节、未消费缓冲区 2 MiB。每次读取一小块并循环调用 `next()`，及时消费数据。

### 第四步：控制流程与 SDP

同一条 TCP 连接上顺序发送五种请求，CSeq 从 1 递增到 5：

| 请求 | URL | 成功后的服务器状态 | 关键数据 |
| --- | --- | --- | --- |
| OPTIONS | `/live` 或 `*` | 保持原状态 | Public 列出五种方法 |
| DESCRIBE | `/live` | 保持原状态 | 返回 SDP、Content-Type 和 Content-Base |
| SETUP | `/live/trackID=0` | Init → Ready | 协商 TCP 通道，返回 Session |
| PLAY | `/live` 或 `/live/` | Ready → Playing | 必须携带正确 Session |
| TEARDOWN | `/live` 或 `/live/` | Ready / Playing → Closed | 回复后关闭连接 |

`RtspServerSession` 不调用 Socket，只根据输入请求生成响应。因此可以直接测试错误请求，而不需要创建连接。错误请求不修改状态；未知资源返回 404、错误 Session 返回 454、错误调用顺序返回 455、不支持的 Transport 返回 461、未实现方法返回 501。

客户端从 SDP 读取 payload type、编码、时钟、fmtp 和 control。轨道 URL 的基地址优先级为 `Content-Base` → `Content-Location` → DESCRIBE 请求 URL，支持绝对 URL、根路径、普通相对路径及聚合标记 `*`。相对地址按 URL 目录规则解析，例如基地址 `/live/` 配合 `trackID=0` 得到 `/live/trackID=0`，基地址 `/live` 则得到 `/trackID=0`。

当前边界：

- SDP 只支持一条 `video`、一个动态 payload type（96～127）及 `H264/90000`，不支持多轨道、多 payload type 或其他编码。
- Transport 只支持单个 `RTP/AVP/TCP;unicast;interleaved=x-y` 方案，可带 `mode=play`；不支持 UDP、multicast 和备选列表。
- 客户端保存 SETUP 响应中的通道号和 Session，后续 PLAY、TEARDOWN 携带该 Session；允许剥离 `Session` 后的 `;timeout=...` 参数。
- URL 采用常用 `rtsp://host[:port]/path` 形式；不支持认证、IPv6 URL、重定向或跨主机建立新连接。control 不支持 `.` / `..` 路径段；示例轨道与会话使用同一服务器。
- 客户端要求 SDP 响应的 Content-Type 为 `application/sdp`；服务器仅处理简单的 Accept 值，不实现完整媒体类型协商。
- 暂不实现超时保活、认证、PAUSE、任意位置 seek、媒体发送及播放同步。服务器不宣称支持 Session timeout；接入媒体文件时再补 SPS/PPS。

### 第五步：内存中的 RTP 与 H.264

本步只增加三个核心部分：

```text
原始 NALU（含 NAL 头，不含 Annex-B 起始码）
    → H264Packetizer：得到一个或多个 H.264 负载
    → 外层填写 RtpPacket：PT、序号、时间戳、SSRC、Marker
    → serialize_rtp_packet：得到 RTP 二进制
    → 已有交织帧封装 / 流拆分
    → parse_rtp_packet：恢复公共头与媒体负载
    → H264Depacketizer：输出完整 NALU
```

建议按 `rtp_packet.h` → `rtp_packet.cpp` → `h264_packetizer.cpp` → `h264_depacketizer.cpp` → `tests/h264_tests.cpp` 的顺序阅读。测试中的 `test_interleaved_pipeline()` 展示了完整链路，逐字节输入交织流，混入 RTSP 响应和 RTCP 通道，并比较还原结果。

一次 NALU 封包与解包的最小用法如下；示例只演示字节处理，数据并非可解码图像：

```cpp
rtsp::H264Packetizer packetizer;
rtsp::H264Depacketizer depacketizer;  // 放在收包循环外，保留未完成的 FU-A。
std::vector<std::string> nalus;
std::string original = std::string("\x65", 1) + "abcdef";
std::vector<std::string> payloads = packetizer.packetize(original, 4);

for (std::size_t i = 0; i < payloads.size(); ++i) {
    rtsp::RtpPacket packet;
    packet.payload_type = 96;  // 正式接入时使用 SDP 协商结果。
    packet.sequence_number = static_cast<std::uint16_t>(100 + i);
    packet.timestamp = 9000;
    packet.ssrc = 12345;
    packet.marker = i + 1 == payloads.size();  // 此例假设该 NALU 是图像的最后一个。
    packet.payload = payloads[i];

    std::string wire = rtsp::serialize_rtp_packet(packet);
    rtsp::RtpPacket received = rtsp::parse_rtp_packet(wire);
    rtsp::H264DepacketizeResult result = depacketizer.push(received, nalus);
    // Waiting / Dropped 时 nalus 为空；Complete 时 nalus 含完整 NALU。
    // 此例最后一次返回 H264Complete，nalus[0] 与 original 完全一致。
    if (result == rtsp::H264Complete) {
        // 后续第六步将这些 NALU 添加起始码并写入文件。
    }
}
```

接口与边界：

- `RtpPacket` 不绑定编码；固定支持 RTP 版本 2。按字节读写公共头，保存 CSRC 和原始扩展数据，解析时剥离 Padding。扩展长度以 4 字节为单位；不解析扩展内部的具体格式。生成 Padding 时除最后一个长度字节外填零，不保留输入包的任意填充值。
- `parse_rtp_packet()` 接收一整个 RTP 包，不含 `$` 头。非法版本、截断和错误 Padding 抛出异常。RTP 层允许空负载，但 H.264 解包器会拒绝空负载。
- `H264Packetizer::packetize()` 输入一条原始 NALU，支持类型 1～23；负载默认上限为 1200 字节，包含 FU-A 的两字节头，但不包含 RTP 或交织头。发送端支持单 NALU 和 FU-A，暂不生成 STAP-A。
- `H264Depacketizer::push()` 每次清空输出数组。单 NALU 输出一条，STAP-A 可输出多条，FU-A 完整后才输出一条。输出仍含 NAL 头、不含起始码。
- 返回值为 `H264Waiting`（等后续片）、`H264Complete`（有完整输出）或 `H264Dropped`（缺少开始片或分片连续性失败）。格式错误会抛出异常，清空输出和重组状态；一个非法 STAP-A 不会泄漏已经拆出的部分 NALU。
- 一个解包器处理一路轨道；不重排、不重传。FU-A 检查连续序号、时间戳、SSRC、PT、原始 NAL 类型及 NRI。序号允许 `65535 → 0` 回绕；新开始片、单 NALU 或 STAP-A 会放弃未完成的旧分片。
- 接收端支持空 FU-A 分片并忽略 R 保留位。完整 NALU 上限为 4 MiB，超限清空状态并报错；`reset()` 可以在连接结束时主动清理。
- 封包器只拆分媒体负载，不维护 RTP 时钟或图像边界。外层为每包递增序号，同一图像保持时间戳一致，只有整幅图像最后一个包置 Marker。FU-A 的 E 位只表示 NALU 结束，解包器不依赖 Marker 判断是否收全。
- TCP 交织通道和 RTP 的 payload type 是两种不同字段。第六步接入时应先按协商通道分流 RTP / RTCP，再检查 RTP 的 PT，随后交给对应媒体解包器。

新增测试为 `rtp_tests` 与 `h264_tests`，已注册至 CTest。本步不依赖媒体文件或 FFmpeg，程序仍只运行第四步的控制流程。

### TCP 接口约定

- 一个 `TcpSocket` 管理一个句柄，监听和连接使用不同对象；禁止复制和移动。析构和 `close()` 都会释放句柄，重复 `close()` 安全。
- `listen()` 用于监听对象，`accept_client(connection)` 将接受的新连接写入空的连接对象；`connect()` 用于客户端连接对象。`send_all()`、`receive()` 和 `shutdown_send()` 应在已连接对象上调用。
- 已打开的对象必须先 `close()` 才能再次 `connect()` 或 `listen()`。`is_open()` 只检查本地是否持有句柄，不判断对端是否在线。
- `send_all()` 循环发送，直到全部字节已交给系统，错误通过异常报告。发送完成不等于对端业务已经处理完成。
- `receive()` 返回实际收到的字节数，返回 `0` 表示对端正常关闭发送方向；缓冲区不能为空。一次读取可能只有部分数据。
- `shutdown_send()` 只关闭发送方向，可以继续接收；保留此接口供 TCP 测试使用。当前 RTSP 程序通过 TEARDOWN 结束会话，不再靠半关闭结束控制流程。
- 当前采用阻塞式调用，连接或收发期间会等待；媒体接入阶段再添加调度循环。

## 1. 第一版目标

- 使用 **C++17 + CMake**，先在 Windows 上运行。
- 实现两个程序：`rtsp_client` 和 `rtsp_server`。
- 使用 **RTSP 1.0 + TCP interleaved（交织传输）**。
- 一个服务器连接一个客户端，先传 H.264 视频，再加入 AAC 音频。
- 服务器读取本地媒体文件；客户端解包后保存文件，用 ffplay 验证。

第一版采用阻塞式 Socket、每个连接一个顺序处理循环。后续再扩展 UDP、并发客户端、认证、完整 RTCP 和音视频播放同步。

## 2. 先分清三件事

| 内容 | 作用 | 本项目负责的部分 |
| --- | --- | --- |
| RTSP + SDP | 协商播放方式、说明媒体格式 | 请求响应、会话、媒体参数 |
| RTP + RTCP | 传输媒体、反馈统计和同步信息 | 先实现 RTP，并识别、分流 RTCP |
| H.264 + AAC | 音视频编码格式 | 读取、封包、解包；解码交给外部播放器 |

TCP 连接里会同时出现两种消息：

```text
RTSP 消息：起始行 + 消息头 + 空行 + 消息体（可选）
媒体消息：'$' + 通道号（1字节）+ 长度（2字节）+ RTP/RTCP包
```

媒体消息的长度使用网络字节序，只计算后面的 RTP/RTCP 包，不包含前面 4 字节。

## 3. 目录与模块

```text
rtsp-cpp/
├── README.md       # 当前进度、使用方式、项目架构与实现顺序
├── CMakeLists.txt  # rtsp_core、两个程序和 CTest
├── src/
│   ├── app/        # 客户端、服务器入口，组织调用流程
│   ├── net/        # TCP Socket，收字节、发字节、关闭连接
│   ├── rtsp/       # RTSP消息、TCP混合消息拆分、会话流程
│   ├── sdp/        # SDP解析与生成
│   ├── rtp/        # RTP公共头解析与生成
│   └── media/      # H.264、AAC封包解包及媒体文件读写
├── tests/          # 协议解析与封包解包测试
└── samples/        # 本地测试媒体文件
```

根目录的 `CMakeLists.txt` 已创建，先编译一个供双方共用的 `rtsp_core` 库，再分别链接客户端和服务器。

每个模块先使用少量 `.h` / `.cpp` 文件，按需要增加，不必提前设计复杂继承体系。

| 模块 | 建议的类或结构体 | 职责边界 |
| --- | --- | --- |
| `net` | `TcpSocket`（当前教学版） | 连接、监听、接收、完整发送；不理解 RTSP |
| `rtsp` | `RtspMessage`（已实现） | RTSP请求/响应解析与序列化，处理 CSeq 和 Content-Length |
| `rtsp` | `RtspStreamParser`（已实现） | 累积 TCP 字节，拆出完整 RTSP 消息或交织帧 |
| `rtsp` | `RtspClient`、`RtspServerSession`（已实现） | 组织 OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN |
| `sdp` | `SessionDescription`、`MediaTrack`（已实现） | 保存 control、编码、payload type、时钟频率和 fmtp 参数 |
| `rtp` | `RtpPacket` | 解析和生成 RTP 头，提供媒体负载 |
| `media` | `H264Packetizer`、`H264Depacketizer` | H.264 NALU 与 RTP 负载之间转换 |
| `media` | `AacPacketizer`、`AacDepacketizer` | 按 SDP 参数处理 AAC AU Header |
| `media` | `H264FileReader`、`AacFileReader` | 读取媒体文件，提供编码数据和时间信息 |

建议先约定三种数据：`MediaTrack` 保存轨道配置，`RtpPacket` 保存 RTP 头与负载，`NalUnit` 保存一个包含 NAL 头、没有 Annex-B 起始码的 NALU。具体字段在实现对应模块时再增加。

Windows 的 `WSAStartup`、`closesocket` 等细节集中放在 `net` 模块。

## 4. 调用关系与数据流

```text
app → rtsp → net
          → sdp
          → rtp
          → media
```

`media` 负责媒体格式，`rtp` 负责公共传输头；由 RTSP 会话把两者连接起来。

服务器发送视频：

```text
H.264文件 → 读取NALU → H264Packetizer → 添加RTP头
         → 添加TCP交织帧头 → TcpSocket发送
```

客户端接收视频：

```text
TcpSocket接收 → RtspStreamParser拆消息 → 解析RTP头
             → H264Depacketizer重组NALU
             → 添加00 00 00 01 → 保存received.h264
```

RTSP 消息进入会话处理，RTCP 消息进入单独分支，不交给 H.264/AAC 解包器。视频与音频使用 SETUP 协商得到的通道号，例如视频 RTP/RTCP 为 `0/1`，音频为 `2/3`。

## 5. 最小交互流程

```text
客户端                                服务器
  | -------- OPTIONS ----------------> | 返回支持的方法
  | -------- DESCRIBE ---------------> | 返回包含视频轨道的SDP
  | -------- SETUP 视频轨道 ---------> | 返回Session和TCP通道号
  | -------- PLAY -------------------> | 开始发送RTP视频包
  | <======= TCP交织媒体帧 =========== |
  | -------- TEARDOWN ---------------> | 释放会话资源
```

客户端使用 SDP 中的 `a=control` 和响应中的基地址解析轨道 URL。SETUP 成功后，后续请求携带服务器返回的 Session。

进入播放循环后，服务器仍要处理客户端请求；可用 `select` 配合下一次发送时间，避免只顾发送而收不到 TEARDOWN。加入会话超时后，再实现按需保活。

## 6. 分阶段实现与验收

下表沿用最初架构文档的六阶段编号；实施路线“第三步”对应表中的第 2 阶段，“第四步”对应第 3 阶段。当前已完成第 3 阶段，以及第 4 阶段中的 RTP / H.264 内存处理；文件收发尚未接入。

| 阶段 | 实现内容 | 完成标准 |
| --- | --- | --- |
| 1（已实现） | CMake、两个入口、TCP连接与收发 | 客户端能连接服务器，双方能传递字节 |
| 2（已实现） | RTSP消息解析、序列化、TCP缓冲拆分 | 能处理半条消息、连续多条消息、带消息体的响应 |
| 3（已实现） | OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN、SDP | 完成一次会话，获得轨道参数、Session和通道号 |
| 4（部分完成） | 已实现交织帧、RTP头、H.264封包解包；待接入文件收发 | 客户端保存的 `received.h264` 能被 ffplay 播放 |
| 5 | 与外部工具互通 | ffplay能播放自制服务器；自制客户端能接收已知H.264 RTSP流 |
| 6 | AAC、基本RTCP、保活与异常退出 | 能接收音频，正确处理断开与超时 |

第4阶段先实现单 NALU、FU-A，再补 STAP-A。服务器暂时可以不发送 STAP-A，客户端补齐后可兼容更多流。

建议视频样例使用 **25 fps、无 B 帧的 H.264 Annex-B 文件**。发送时以一个访问单元（可先理解为一幅编码图像）为单位推进时间：同一图像的所有 RTP 包使用相同时间戳，最后一个包的 Marker 为 1；下一幅图像的时间戳增加 `90000 / 25 = 3600`。一个图像可能包含多个 NALU，不能把每个 NALU 都当成一帧。

完成视频收包后可验证：

```powershell
ffplay -f h264 received.h264
```

服务器完成后可用外部客户端验证：

```powershell
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

上述媒体命令用于对应阶段实现完成后验收；当前可运行程序的用法见本文开头的“当前实现与运行”。

## 7. 实现时记住这几条

- **TCP是字节流**：一次接收可能只有半条消息，也可能有多条；发送也可能只完成一部分，需要循环发送。
- **先拆完整消息，再解析内容**：RTSP按空行和 Content-Length 拆分；交织帧按4字节头中的长度拆分，不能扫描二进制负载里的 `$` 来找边界。
- **RTP头长度不固定**：正确处理 CSRC、扩展头和尾部 Padding，检查长度后再访问数据。
- **H.264重组要检查连续性**：FU-A 丢片或序号不连续时丢弃不完整 NALU；单 NALU、STAP-A、FU-A 分别处理。
- **AAC根据SDP解析**：先支持 MPEG4-GENERIC / AAC-hbr；AU Header 按 SizeLength 等参数拆位。ADTS文件输入需去掉ADTS头再封装，保存ADTS文件时则根据音频配置补头。
- **音视频分别计时**：H.264时钟为90 kHz，AAC时钟通常为采样率。常见AAC-LC一帧是1024个采样，和AU-size表示的压缩数据字节数不是一个概念。

先用固定字节样例验证解析器，再用 Wireshark 检查交互和 RTP 序号、时间戳。封包后再解包，应能还原原始 NALU 或 AAC 数据。

## 8. 协议资料

- [RTSP 1.0：RFC 2326](https://www.rfc-editor.org/rfc/rfc2326)
- [SDP：RFC 8866](https://www.rfc-editor.org/rfc/rfc8866)
- [RTP / RTCP：RFC 3550](https://www.rfc-editor.org/rfc/rfc3550)
- [H.264 RTP封装：RFC 6184](https://www.rfc-editor.org/rfc/rfc6184)
- [MPEG-4 Generic / AAC RTP封装：RFC 3640](https://www.rfc-editor.org/rfc/rfc3640)

按当前阶段查阅相关部分即可：先读懂消息和包结构，再逐步补齐协议细节。


## 9. 实施路线（八步）

下面使用对话中的八步编号，与前面保留的六阶段表区分。

- [x] **1．建立可编译的工程骨架。** C++17、CMake、共用库、两个入口和测试目标。
- [x] **2．完成 TCP 收发。** 使用教学版 `TcpSocket` 封装监听、连接、完整发送、接收、半关闭和自动关闭，验证二进制回显。
- [x] **3．完成 RTSP 消息与 TCP 混合流解析。** 按空行、Content-Length 和交织帧长度拆分，验证半包、粘包、消息体、非法输入与缓冲上限。
- [x] **4．打通 RTSP 控制流程与 SDP。** 顺序完成五种请求，取得轨道参数、Session 和协商通道；错误 Session、Transport 和调用顺序会被拒绝；当前尚无媒体数据。
- [x] **5．在内存中验证 RTP 与 H.264 封包解包。** 已实现 RTP 公共头、单 NALU / FU-A 封包，单 NALU / FU-A / STAP-A 解包；验证固定字节、逐字节还原、截断、缺片恢复、序号回绕、CSRC、扩展、Padding、重组上限及交织流分流。
- [ ] **6．接入媒体文件，完成视频传输闭环。** 读取 Annex-B，按访问单元组织发送，客户端写出 `received.h264`；先使用 25 fps、无 B 帧、带 AUD 的样例。验证时间戳、Marker、文件可播放和播放中的 TEARDOWN。
- [ ] **7．使用外部工具验证互通。** ffplay 连接自制服务器，自制客户端接收已知 H.264 RTSP 流；检查 SDP、SPS/PPS、轨道地址和通道协商，防止双方共同错误被自测掩盖。
- [ ] **8．加入 AAC 和会话健壮性。** 支持 MPEG4-GENERIC / AAC-hbr、AU Header、ADTS 转换，随后补保活、超时、基本 RTCP、文件结束和异常断开；音视频分别维护时钟和序号。

