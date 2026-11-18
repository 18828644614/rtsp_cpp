# 独立音频编码实现笔记

## 已核实基础
- AudioSource、FileAudioSource、MicrophoneCapture 和抽象音频源会话构造已经存在。
- CameraVideoSource 提供可复用的工作线程、启动等待和异常传播组织模式。
- 必须由消费队列的 read 确认结束，避免生产者完成与最后一帧入队之间的竞态。
- DirectShow 打开、读取和关闭保持在同一个工作线程。
- 构建环境已有 Visual Studio 2022 和 FFmpeg 9.0.2 shared SDK。
- FFmpeg 编码依赖保持可选，不使 rtsp_core 依赖 FFmpeg。

## 验收证据
- 原生 AAC 首包 PTS=-1024，initial_padding=1024。
- 合成输入 44117 采样（44100 Hz、立体声），重采样输出 48019，提交 48128，尾补 109。
- 外部 ADTS 解码得到 49152 个采样；其数量等于 initial_padding + 有效采样 + 尾补。
- 外部 RTSP 测试发现 FFmpeg 使用 base_timestamp=0 表示未初始化，首次 RTCP SR 会把零起点改成 SR 的 RTP 时间，造成开头几个 tick 的 PTS 偏移。
- 网络音频轨道采用独立非零 RTP 起点（复用该轨道 SSRC 值）；订阅内部保留显式可配置起点，测试覆盖负 AAC PTS 和 uint32_t 回绕。
- FFmpeg 实现依据：https://ffmpeg.org/doxygen/trunk/rtpdec_8c_source.html （finalize_packet / handle_rtcp_packet）。
- 外部 FFmpeg 解码脉冲实际位于 13024，预期 12000+1024，完全一致。
- 默认 CTest 17/17、AAC+采集 31/31、摄像头/H.264/麦克风/AAC 全开 40/40。
- 无麦克风采集依赖时，仅 AAC 构建和三个关键编码/源/外部 RTSP 测试通过。
