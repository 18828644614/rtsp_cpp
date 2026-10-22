# 第六步：H.264 文件传输闭环

## 1. 本步目标

服务器读文件，经 RTSP/TCP 连接发送，客户端解包后保存。编码数据保持文件中的解码顺序，不识别或重排 I/P/B 图像；每 40 ms 发送一个访问单元。

不增加继承、线程、回调或 FFmpeg 库。FFmpeg 命令仅用于准备样例和解码验收。

## 2. 模块架构

```text
服务器 server_main
  H264FileReader：打开文件、拆 NALU、按 AUD 组织访问单元
  SessionDescription：保存从文件生成的 SDP 参数
  RtspServerSession：请求检查、状态切换、生成响应
  发送循环：等待控制请求，或到时发送一个访问单元
    H264Packetizer → RtpPacket → TCP 交织帧 → TcpSocket

客户端 client_main → RtspClient
  OPTIONS → DESCRIBE → SETUP → PLAY
  RtspStreamParser：拆出 RTSP 消息和交织帧
    RTP 通道 → parse_rtp_packet → H264Depacketizer → H264FileWriter
    RTCP 通道 → 暂时跳过
  文件 EOF：检查残留半包与未完成重组，关闭文件
  达到接收上限：发送 TEARDOWN，等待响应后关闭文件
```

## 3. 新增的两个类

### H264FileReader

- `open(path)`：一次读入小样例并检查，支持三字节或四字节 Annex-B 起始码。
- `read(frame)`：依次输出一个 `H264AccessUnit`；每条 NALU 含 NAL 头，不含起始码。
- `sps()` / `pps()`：提供不带起始码的参数集，供 SDP 生成使用。
- `frame_count()`：访问单元总数，用于日志和检查。

文件上限为 64 MiB，单条 NALU 上限为 4 MiB。依据 AUD 而不是 NAL 类型 1/5 划分图像，所以多 slice 图像不会被拆成多帧。文件结束时输出最后一个访问单元，不要求末尾还有 AUD。

输入必须保证每个访问单元都有 AUD；读取器不解析 slice header、真实帧率或 PTS/DTS。首组 SPS/PPS 用于 SDP，参数集仍按文件顺序通过 RTP 发送。

### H264FileWriter

- `open(path)`：二进制方式创建或覆盖输出文件。
- `write(nalu)`：写入四字节起始码和一条完整 NALU。
- `close()`：关闭文件并报告写入或关闭错误。

不接触 Socket、不解释 RTP，也不处理未完成的 FU-A。

## 4. 服务器的发送规则

`send_frame()` 是入口文件中的普通函数，遍历访问单元内的所有 NALU，调用已有封包器。

| 数据 | 更新规则 |
| --- | --- |
| RTP Sequence | 每发送一个包增加 1，允许 16 位回绕 |
| RTP Timestamp | 每发送一个访问单元增加 3600，允许 32 位回绕 |
| Marker | 仅访问单元最后一个 NALU 的最后一个包置 1 |
| SSRC | 单路教学样例固定为 12345 |
| PT | 读取 SessionDescription 配置 |
| RTP 通道 | 读取 SETUP 协商结果，不写死为 0 |

PLAY 响应发完后才发送媒体。下一次计划时刻增加 40 ms，避免封包耗时不断累加到间隔中。`wait_readable()` 用 `select()` 在发送间隙等待请求。文件发完后关闭发送方向并退出；客户端提前发送 TEARDOWN 时先回复再退出。`send_all()` 仍然阻塞，慢客户端可能使控制响应延迟。

## 5. 客户端的接收规则

`exchange()` 等待 RTSP 响应，`receive_media()` 持续收媒体；两者共用 `handle_interleaved()` 和同一个流解析器，所以 PLAY 响应与媒体粘包、TEARDOWN 响应前的在途媒体不会直接被跳过。

处理交织帧时先根据通道区分 RTP/RTCP，再检查 PT、序号和 SSRC，最后解包并写入完整 NALU。EOF 时检查解析器残留、未完成 FU-A 和最后一个 Marker；空流不算成功。但在完整图像边界提前断开时，单靠 EOF 仍无法证明收到所有图像，需要比较数量或内容。

不指定 `frame_limit` 时接收到 EOF。指定正数时，在达到数量且访问单元完整后发 TEARDOWN；等待响应期间继续处理在途媒体，输出可能略多于上限。

## 6. B 帧与时间戳

允许 B 帧，外部解码器负责恢复显示顺序。当前 `timestamp += 3600` 是实验简化，不是 B 帧真实 PTS，也不保证通过 RTSP 直接实时播放时的同步。以后做标准实时互通，再从容器读取 PTS/DTS，分别用于 RTP 时间戳和发送调度。

## 7. 阅读顺序与验收

1. `src/media/h264_file_reader.h/.cpp`：文件输入与访问单元。
2. `src/media/h264_file_writer.h/.cpp`：输出文件。
3. `src/app/server_main.cpp`：文件准备、控制循环和发送函数。
4. `src/rtsp/rtsp_client.cpp`：交织帧处理、媒体循环和退出。
5. `tests/media_file_tests.cpp`、`tests/media_integration.ps1`：文件与真实连接验证。

运行命令见 README，样例生成见 `samples/README.md`。CTest 使用手写编码字节验证传输，不声称测试字节可以解码。

本机另行验证了 2 秒、50 幅图像的真实样例，包含 2 个 I、18 个 P、30 个 B；传输前后的解码逐帧 MD5 完全一致。原文件与输出的起始码长度可能不同，不能只凭整个文件哈希判断 NALU 是否一致。
