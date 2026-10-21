# 学习记录（Learnings）

## [LRN-20261021-001] correction

**Logged**: 2026-10-21
**Priority**: high
**Status**: resolved
**Area**: backend

### 摘要（Summary）
初学者项目应先突出 TCP 调用顺序，避免同时引入移动语义、静态工厂、友元和智能指针删除器。

### 详情（Details）
用户要求代码简单清晰，但第一版封装需要先理解较多 C++ 所有权机制。用户在学习中明确要求简化。

### 建议行动（Suggested Action）
先创建对象，再调用普通成员函数。保留正确收发和自动关闭；接受连接通过引用输出，测试使用顺序调用减少线程知识负担。

### 元数据（Metadata）
- Source: user_feedback
- Related Files: src/net/tcp_socket.h, src/net/tcp_socket.cpp, tests/tcp_tests.cpp
- Tags: teaching, simplicity, tcp

### 解决情况（Resolution）
已改为普通成员函数和引用输出，只保留一个 Socket 类；端口解析及测试同步简化，编译和回显验证通过。
