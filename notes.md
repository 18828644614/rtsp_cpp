# 实现记录

## 阶段 6：事件循环与非阻塞网络（2026-10-27）

- 当前 wait_readable 仅 select 单个 Socket；连接仍各有接收/发送线程。
- MediaSession::run 的预读帧、时钟原点需要成为成员，tick 只推进到期数据，空订阅暂停。
- 旧测试直接启动连接线程/等待媒体线程，需要改为显式驱动，保留原行为断言。
- 输出必须记住当前消息与偏移；在途消息仍计入有界队列，完整发送后释放计数。
- select 最大 FD_SETSIZE 个 Socket（包含监听）；只为存在输出的连接关注可写。
- 所有服务器 Socket 的创建后操作/关闭都由运行循环执行；外部 stop 只设置原子标记。
- 已实现 EventLoop、非阻塞收发、每连接部分发送状态、媒体 tick/next_deadline，服务器不再创建连接或媒体线程。
- 在途消息仍占队列额度；Open/Draining/Closed 路径支持正常排空与异常关闭；控制空闲 60 秒、发送无进展 2 秒回收。
- 原有 11 组行为测试迁移后通过，新增专项测试验证 8 MiB 真实背压续传、约 10 MiB FU-A 慢连接隔离、半关闭响应排空、媒体期限/预算、63 客户端上限及停止。
- 最终 MSVC Debug 构建成功，12/12 CTest 通过（14.26 秒）；文档链接与代码围栏检查通过。

## TCP 多轨道与 AAC（2026-10-26）

- 当前 SessionDescription、RtspServerSession、MediaSubscription、RtpSender 均存在单 H.264 假设，需要顺序扩展。
- 同一客户端所有轨道共用一个 OutboundQueue；SETUP 状态按轨道保存，序号和 SSRC 按订阅的轨道保存。
- 当前视频固定 25 fps、90 kHz，不解析真实 PTS/DTS；音视频验证使用无 B 帧且起点一致的素材。
- 保持自带客户端保存 H.264 的功能：解析多轨道 SDP，但只 SETUP 所选视频轨道。
- 同步调度采用累计 PTS 换算绝对时刻，避免累加截断的 AAC 帧间隔。完整 RTCP SR 和真实容器时间戳属于后续范围。
- 已通过 11 组 CTest；外部 FFmpeg 8.1 经 TCP 接收 75 帧视频，解码逐帧 MD5 一致；48 kHz 双声道 AAC 解码 PCM 共 581632 字节，SHA-256 与源文件一致。纯音频和自带视频客户端均通过。
- 外部互通发现既有 Range 校验只接受 `npt=0-`，现支持等价的 `npt=0.000-`，继续拒绝非零 seek。本机 FFmpeg RTSP 输入不接受 `-rw_timeout`，验证脚本使用进程级超时。
- 运行中出现的 `.obsidian/workspace.json` 修改属于外部编辑器状态，本次不修改或还原。

## 第六步：H.264 文件与视频收发

- 保留普通结构体、函数和顺序循环；只新增 H264FileReader、H264FileWriter 和 H264AccessUnit。
- 文件一次读入，限制 64 MiB；按 AUD 组织图像，兼容三/四字节起始码，提取首组 SPS/PPS 生成 SDP。
- 服务器使用 select 可读等待与 steady_clock 绝对计划时刻交替处理控制请求和媒体，每 40 ms 发送一个访问单元。
- 客户端等待响应和持续收媒体共用交织帧处理逻辑；正常 EOF 检查半包、未完成 FU-A 和最后一个 Marker；提前结束仍发 TEARDOWN。
- 允许 I/P/B，保持解码顺序；按发送序号生成占位 RTP 时间戳，不宣称已经实现真实 PTS/DTS 或标准实时显示同步。
- CMake/MSVC Debug 和 Release 构建成功，各七组 CTest 全部通过，Debug 集成测试额外连续三轮通过。实际含 30 个 B 帧的 50 帧视频传输后，FFmpeg 解码的逐帧 MD5 与原文件一致。
- 本次工具问题：PowerShell 包装脚本无法正确传递多行补丁，改用原生 apply_patch 工具；服务器首次构建缺少 rtp_packet.h，已补齐。补丁模板的转义与一次文档匹配错误均在写入前被拒绝，随后分开提交成功。

## 第五步：RTP 与 H.264 内存处理

- 新增三个模块：RtpPacket（普通结构体及解析/生成函数）、H264Packetizer（无状态封包）、H264Depacketizer（单路分片状态）。
- 二进制继续用 std::string 保存，按字节读写，不增加模板、回调、继承、智能指针或线程。
- RTP 检查版本、CSRC 长度、扩展字数和 Padding；生成端校验字段范围，扩展不解释内部语义。
- H.264 发送单 NALU / FU-A，接收单 NALU / FU-A / STAP-A；不生成 STAP-A。
- FU-A 检查序号（允许回绕）、时间戳、SSRC、PT、类型及 NRI；异常连续性返回 H264Dropped，新开始片可以恢复。格式错误清空状态后抛异常。
- NALU 上限 4 MiB；STAP-A 全部合法才输出。支持空 FU-A 分片，接收端忽略 FU R 位。
- 序号、时间戳和 Marker 由外层填写；媒体模块不读文件、不调用 Socket，两个程序仍运行控制流程。
- 新增 rtp_tests、h264_tests 并注册 CMake；两组测试直接编译运行通过，固定字节与完整内存链路均验证。
- TCP、RTSP、会话三组测试已重新编译运行通过。
- 客户端和服务器重新编译通过，实际双进程完成 OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN 并正常退出。
- 所有编译启用 C++17、常用警告及 -Werror，无警告。CMake/CTest 配置已更新，本次未实际执行这些命令。
- README 增加模块职责、完整内存数据流、最小调用示例、返回状态、大小限制和阅读顺序。

## 初始情况

- 原项目只有 README 和空模块目录。
- 此前“第 1、2 点”指工程骨架和 TCP 收发，不包含 RTSP 消息解析。
- net 模块封装 Windows Socket，最初程序入口只组织回显流程。
- 测试使用回环地址和系统分配的端口，不依赖媒体文件或外部服务。
- 早期回显通过关闭发送方向表示输入结束；第三步已改用 RTSP 的 Content-Length 拆分，半关闭仅用于演示结束后的连接释放。

## 初版验证结果（简化前）

- 客户端、服务器和测试程序均使用现有 g++ 直接编译通过，启用 `-std=c++17 -Wall -Wextra -Wpedantic -Werror`。
- `tcp_tests` 三组检查通过：非法参数与关闭状态、移动所有权与析构释放、2 MiB + 37 字节二进制传输与半关闭/EOF。
- 客户端和服务器的 `--help` 及非法端口检查通过。
- 双进程回显 `Stage 1 and 2: TCP echo` 成功，客户端校验 23 字节一致，双方正常退出。
- 未安装任何工具或依赖，未使用媒体文件。CMake/CTest 命令未实际执行，构建配置已提供。
- 验证产物保存在 `.verify/`，已加入 `.gitignore`。

## 教学版简化

- 去掉 `TcpListener`，只保留一个保存原始句柄的 `TcpSocket` 类。
- 使用 `connection.connect(...)`、`listener.listen(...)` 和 `listener.accept_client(connection)`，无需构造并返回资源对象。
- 去掉移动语义、友元、智能指针删除器、主机名解析和测试线程。保留禁止复制、析构关闭、完整发送及错误检查。
- `parse_port` 改成逐位计算并即时检查范围；IPv4 转换和系统 Socket 创建的结果直接写入成员或地址结构。
- 当前只接受数字 IPv4 地址，`localhost` 不再支持，README 与帮助已同步。
- 测试改为同线程分块收发，验证 2 MiB + 37 字节、半关闭、EOF、对象复用和析构关闭。
- 三个程序均直接编译通过（`-std=c++17 -Wall -Wextra -Wpedantic -Werror`），测试和双进程回显通过。CMake/CTest 配置已同步，未实际运行这些命令。

## 第三步：RTSP 消息与混合流拆分

- 沿用简化后的 Socket，不增加复杂资源封装、模板、线程或回调。
- 新增 `RtspMessage`、`RtspHeader`：请求和响应共用结构体，头字段存入顺序数组；按大小写无关的字段名查询与修改。
- 文本解析按 CRLF、空行和 Content-Length 拆分，校验数字 CSeq、重复关键字段、控制字符和长度上限。不完整输入不修改输出。
- 新增 `RtspStreamParser`：按当前起点分流 RTSP 与 '$' 交织帧，保留半包，循环取出粘在一起的多条数据；EOF 检查截断。
- 交织帧保留通道号和原始负载，不解释 RTP/RTCP；服务器尚无 SETUP，收到帧会报错。
- 两个入口改为最小 OPTIONS 演示，响应回传 CSeq；其他方法返回 501，完整 SDP/会话逻辑仍留到下一步。
- 文本仅支持 RTSP 1.0、CRLF 和单行头字段。消息头上限 16 KiB，消息体上限 1 MiB，未处理缓冲上限 2 MiB。
- README 更新了阅读顺序、调用示例、接口限制、命令和阶段编号的对应关系。

### 验证结果

- 客户端、服务器、TCP 测试及 RTSP 测试均用已有 g++ 直接编译，开启 C++17、常用警告和 -Werror，无编译警告。
- RTSP 四组测试通过：文本解析/生成、非法输入、所有分割点与逐字节混合流、大小限制与 EOF。
- 验证二进制消息体、最大 65535 字节交织帧、1 MiB 消息体、缺失/重复 CSeq、重复或非法 Content-Length 以及输出换行注入。
- 既有 TCP 测试回归通过。
- 双进程客户端/服务器 OPTIONS 成功，双方正常退出。
- 使用独立 TCP 客户端验证：带五字节消息体的请求先发半包，再接续剩余消息体及两条请求；服务器未提前回复，依次返回 CSeq 10、11、12，对 DESCRIBE 返回 501。
- CMake 已注册 `rtsp_tests`，未实际运行 CMake/CTest 命令；未安装工具或依赖。

## 第四步：RTSP 控制流程与 SDP

- 已有第三步解析器，本次按八步路线实现控制流程，对应原六阶段表中的第 3 阶段。
- 保留简化后的 TcpSocket 和解析器；使用普通结构体、函数、枚举、字符串和循环，不引入模板、回调、智能指针或线程框架。
- 新增 `MediaTrack` / `SessionDescription`，仅支持单条 H.264/90000 视频、一个动态 payload type，保存 control 和 fmtp；解析时不依赖默认值掩盖缺失参数。
- 新增 TCP Transport 解析与生成，支持 unicast、两个不同交织通道及可选 mode=play；明确拒绝 UDP、multicast、备选列表及重复参数。
- 控制 URL 支持绝对地址、根路径、普通相对路径及聚合 '*'；相对地址遵循目录规则。基地址优先 Content-Base，其次 Content-Location，最后 DESCRIBE 请求 URL。
- 新增 `RtspServerSession`，只负责请求响应与 Init / Ready / Playing / Closed 状态，不直接调用 Socket；错误请求不改变状态。
- 新增 `RtspClient`，在同一条连接上完成五种请求，递增 CSeq、校验响应、保存服务器返回的 Session 与通道，并为后续请求携带 Session。
- 服务器保留单连接模式；TEARDOWN 回复后主动退出，不再等客户端半关闭。已协商通道的帧先跳过，SETUP 前或未知通道的帧报错。
- 当前不发送媒体，不读取文件；SDP 尚不提供来自媒体的 SPS/PPS。不实现认证、多轨道、超时保活、重定向、PAUSE 或任意位置 seek。
- README 明确了上述边界，修正了六阶段与八步路线的编号对应，标记前四步完成。

### 验证结果

- 客户端、服务器、TCP 测试、RTSP 测试和会话测试均使用现有 g++ 直接编译，启用 C++17、常用警告及 -Werror，无编译警告。
- `session_tests` 验证手写 SDP、非默认 payload type、SDP 缺项、URL 解析、非法路径、Transport、错误 Session、重复关键字段、Range、错误状态及正常结束。
- 原有 TCP 和 RTSP 解析测试回归通过。
- 实际客户端和服务器完成 CSeq 1～5 的五步交互，协商 H264/90000、Session 与 0/1 通道，双方正常退出。
- 独立 PowerShell 模拟服务器验证客户端七种场景：Content-Base、Content-Location、请求 URL 回退、错误 CSeq、改变 Session、错误 Transport、提前 EOF。成功场景使用 payload 104、Session 参数、10/11 通道和夹在控制响应之间的交织帧。
- 独立原始 TCP 客户端验证服务器：拒绝无 Session 的 PLAY 和 UDP SETUP 后仍可继续正常会话；协商 6/7 通道；跳过已知 RTCP 通道帧；TEARDOWN 后无需客户端 EOF 即正常关闭。
- 验证工具与可执行产物在忽略目录 `.verify/`，正式会话单元测试已注册到 CMake。未实际执行 CMake/CTest，未安装工具或使用 FFmpeg。
