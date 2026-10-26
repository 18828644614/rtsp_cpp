![[Pasted image 20261022145624.png]]

![[Pasted image 20261022151401.png]]

// 阶段 3：共享 H.264 MediaSession

1. 服务器 rtsp_server 构造函数创建 `MediaSession`，每个 `RtspServerConnection` 都拿到同一个 `media_session_`
2. mediaSession 构造函数创建媒体源，并启动 `run()` 线程。此时线程已经启动，但还没有订阅者，不会推进视频