# 错误记录（Errors）

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
