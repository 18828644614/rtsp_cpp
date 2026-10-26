# 错误记录（Errors）

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
