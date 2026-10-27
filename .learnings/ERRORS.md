# 错误记录（Errors）

## [ERR-20261027-001] event-loop-validation

**Logged**: 2026-10-27
**Priority**: low
**Status**: resolved
**Area**: tests

### 摘要（Summary）
事件循环迁移时请求日志从 std::endl 改为换行，重定向后未即时刷新，导致集成测试无法观察 PLAY；实际客户端已收到全部 20 帧。恢复即时刷新，继续验证。另一次 rg 使用未展开的 Windows 路径通配符，后续改用目录加 -g 过滤。

### 原始错误（Error）
```text
first client did not PLAY
rg: src/media/media_session.*: 文件名、目录名或卷标语法不正确。 (os error 123)
```

### 建议修复（Suggested Fix）
保留测试使用的外部可观察日志及刷新行为；Windows 下将文件过滤模式通过 rg -g 传入。

专项测试补充：首次编译缺少 media/h264_depacketizer.h，补齐后成功。最初仅限制 SO_SNDBUF，2 MiB 回环数据被接收端缓冲全部吸收，导致 `test did not exercise send backpressure`；同时限制 SO_RCVBUF 并发送 8 MiB 后，真实 WouldBlock 和逐字节续传校验通过。测试不再假设出现背压前一定已有发送进展。

### 元数据（Metadata）
- Reproducible: yes
- Related Files: src/rtsp/rtsp_server_connection.cpp, tests/media_integration.ps1

## [ERR-20261022-002] step6-validation

**Logged**: 2026-10-22
**Priority**: low
**Status**: resolved
**Area**: tests

### 摘要（Summary）
第六步实施时的补丁传参和首次编译问题均已修复，没有覆盖原有未提交记录。

### 原始错误（Error）
```text
Error: --codex-run-as-apply-patch requires a UTF-8 PATCH argument.
Invalid patch: The last line of the patch must be '*** End Patch'
Octal escape sequences are not allowed in template strings
SyntaxError: missing ) after argument list
ReferenceError: POWERSHELL_EXECUTABLE is not defined
ReferenceError: crlf is not defined
apply_patch verification failed: Failed to find expected lines
error C2039: "RtpPacket": 不是 "rtsp" 的成员
```

### 解决情况（Resolution）
改用原生 apply_patch 工具提交补丁，避免多层 shell、JavaScript 与 Markdown 的转义冲突；给发送入口补充 rtp/rtp_packet.h，后续构建和测试通过。

### 元数据（Metadata）
- Reproducible: yes
- Related Files: src/app/server_main.cpp, tests/media_integration.ps1

---

## [ERR-20261021-001] apply_patch

**Logged**: 2026-10-21
**Priority**: low
**Status**: resolved
**Area**: infra

### 摘要（Summary）
一次补丁中删除并新增同一路径被拒绝，未修改文件。

### 原始错误（Error）
```text
apply_patch verification failed: invalid patch: multiple operations target E:\MarkDown\rtsp-cpp\src\net\tcp_socket.h
```

### 建议修复（Suggested Fix）
优先使用更新操作；需要整体替换时将删除和新增分为顺序执行的独立补丁。

### 元数据（Metadata）
- Reproducible: yes
- Related Files: src/net/tcp_socket.h

### 解决情况（Resolution）
已拆成独立补丁并成功写入新版代码。
## [ERR-20261026-001] computer-use-native-pipe-unavailable

**Logged**: 2026-10-26T10:00:00+08:00
**Priority**: low
**Status**: pending
**Area**: infra

### 摘要（Summary）
排查 VS Code 跳转时，桌面控制通道不可用，无法自动重载窗口并验证 F12。

### 原始错误（Error）
```text
Computer Use native pipe is unavailable: failed to connect native pipe: 系统找不到指定的文件。 (os error 2)
```

### 上下文（Context）
`sky.list_windows()` 重试和 JavaScript 会话重置后仍失败；项目配置已修正，CMake 配置及 JSON 检查通过，交由用户执行 `Developer: Reload Window` 验证跳转。

### 建议修复（Suggested Fix）
后续使用桌面验证前先确认 Computer Use 原生通道可用；通道不可用时保留文件检查结果并明确实际 UI 尚未验证。

### 元数据（Metadata）
- Reproducible: yes
- Related Files: .vscode/settings.json

---

## [ERR-20261022-001] tshark-capinfos-unavailable

**Logged**: 2026-10-22T00:00:00+08:00
**Priority**: low
**Status**: pending
**Area**: infra

### 摘要（Summary）
系统未安装或未配置 `tshark` 和 `capinfos`，无法使用 Wireshark 命令行工具直接读取 pcapng。

### 原始错误（Error）
```
The term 'capinfos' is not recognized as a name of a cmdlet, function, script file, or executable program.
The term 'tshark' is not recognized as a name of a cmdlet, function, script file, or executable program.
```

### 上下文（Context）
尝试读取 `D:\rtsp_learn\pcapng\connect_3.pcapng` 的 frame 7 和 TCP 流；随后使用 Python 标准库手动解析 pcapng、IPv4 和 TCP 数据。

### 建议修复（Suggested Fix）
如需命令行复现 Wireshark 分析，安装 Wireshark 的 `tshark`/`capinfos` 并将安装目录加入 PATH。

### 元数据（Metadata）
- Reproducible: yes
- Related Files: D:\rtsp_learn\pcapng\connect_3.pcapng
- Tags: wireshark, tshark, pcapng
- See Also: 

---

## [ERR-20261026-001] ffmpeg-rtsp-range-validation

**Logged**: 2026-10-26T16:38:27+08:00
**Priority**: medium
**Status**: resolved
**Area**: backend

### 摘要（Summary）
外部 FFmpeg 验证发现服务器过度限制 Range 表达，合法零起点请求被拒绝；验证脚本还使用了本机 RTSP 输入不接受的参数。

### 原始错误（Error）
```text
method PLAY failed: 457 (Invalid Range)
Option rw_timeout not found.
```

### 上下文（Context）
运行 `python .verify/verify_multitrack.py`，使用 FFmpeg 8.1 经 TCP 接收 H.264 + AAC。现有自制客户端不发送 Range，因此此前自测没有覆盖外部工具发送 `npt=0.000-` 的情况。

### 建议修复（Suggested Fix）
兼容等价的零起点小数表示，继续拒绝非零 seek；保留固定协议样例与外部解码校验。验证脚本移除 `-rw_timeout`，改用进程级超时。

### 元数据（Metadata）
- Reproducible: yes
- Related Files: src/rtsp/rtsp_server_session.cpp, tests/session_tests.cpp, .verify/verify_multitrack.py

### 解决情况（Resolution）
- **Resolved**: 2026-10-26T16:38:27+08:00
- **Notes**: 已增加小数零起点与非零小数拒绝测试；外部双轨道、纯音频及自带视频客户端传输后解码校验通过。

---
## [ERR-20261026-002] ffplay-smoke-log-command

**Logged**: 2026-10-26T20:20:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### 摘要（Summary）
用于启动服务器、启动 ffplay 并合并重定向输出的 PowerShell 命令被执行策略拦截，未影响项目代码或运行结果。

### 原始错误（Error）
```text
exec_command failed: CreateProcess { message: "Rejected(... ) rejected: blocked by policy" }
```

### 上下文（Context）
尝试用 `Start-Process` 同时重定向 ffplay 标准输出和标准错误，并在超时后结束进程；命令字符串包含多段 PowerShell 组合逻辑。

### 建议修复（Suggested Fix）
将验证拆成更短的独立命令，避免复杂重定向和组合脚本；优先查看服务器日志与 ffplay 退出状态。

### 元数据（Metadata）
- Reproducible: unknown
- Related Files: build/ffplay-smoke-server3.log
- Tags: powershell, ffplay, smoke-test

---
## [ERR-20261026-003] utf8-comment-edit

**Logged**: 2026-10-26T20:35:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### 摘要（Summary）
用 Python 替换 C++ 中文注释时，Python 使用系统 GBK 默认编码读取 UTF-8 源文件失败，未写入文件。

### 原始错误（Error）
```text
UnicodeDecodeError: 'gbk' codec can't decode byte 0x80 in position 577: illegal multibyte sequence
```

### 上下文（Context）
尝试修改 `src/rtsp/rtsp_server_connection.cpp` 的说明注释；此前对代码行的补丁修改已经成功，失败发生在后续注释替换步骤。

### 建议修复（Suggested Fix）
读写项目源文件时始终显式指定 `encoding='utf-8'`。

### 元数据（Metadata）
- Reproducible: yes
- Related Files: src/rtsp/rtsp_server_connection.cpp
- Tags: python, encoding, utf8

---
## [ERR-20261026-004] rebuild-locked-server-exe

**Logged**: 2026-10-26T20:38:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### 摘要（Summary）
重新构建时，正在运行的 `rtsp_server.exe` 锁定了输出文件，MSBuild 无法覆盖可执行文件。

### 原始错误（Error）
```text
LINK : fatal error LNK1168: 无法打开 D:\rtsp_learn\rtsp_cpp\build\Debug\rtsp_server.exe 进行写入
```

### 上下文（Context）
服务器进程 PID 4368 仍在运行；需要先停止旧服务器，再重新构建并启动新版本。

### 建议修复（Suggested Fix）
构建前关闭项目服务器；Windows 下可使用 `Stop-Process -Name rtsp_server -Force` 后重新执行 CMake 构建。

### 元数据（Metadata）
- Reproducible: yes
- Related Files: build/Debug/rtsp_server.exe
- Tags: cmake, msvc, file-lock

---
