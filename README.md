# 摄像头 + 麦克风：C++ RTSP 学习版

这个项目只做一件事：**在 Windows 上采集一台摄像头和一支麦克风，通过同一个 RTSP 地址直播。** 正式程序只有 `rtsp_server`，不再读本地 H.264/AAC 文件，不再提供单轨或合成直播模式。

~~~text
摄像头 ─→ H.264 编码 ─→ 视频队列 ┐
                                 ├─→ 按时间戳调度 ─→ RTP/RTCP ─→ RTSP/TCP ─→ 播放器
麦克风 ─→ AAC 编码   ─→ 音频队列 ┘
~~~

## 构建

需要 Windows、支持 C++17 的编译器（下例用 Visual Studio x64）、CMake 和同架构的 **FFmpeg shared 开发包**。开发包应有 `include/`、`lib/`、`bin/`，包含 DirectShow 和 `libx264`；只有 `ffmpeg.exe` 不够。使用新构建目录，避免与旧版多目标的构建产物混在一起。

~~~powershell
cmake -S . -B build-live -A x64 -DFFMPEG_ROOT="C:/ffmpeg-shared"
cmake --build build-live --config Debug
~~~

没有四个分别启用采集、编码的开关。默认也构建测试；只想生成正式服务器时，配置命令可加 `-DBUILD_TESTING=OFF`。构建会把 FFmpeg DLL 复制到服务器旁边；播放用的 `ffplay.exe` 可从 FFmpeg 的 `bin/` 启动或加入 PATH。

## 运行

在 PowerShell 枚举设备，然后把下例的两个 `0` 换成你看到的编号。摄像头和麦克风分别从 0 编号。

~~~powershell
.\build-live\Debug\rtsp_server.exe --list-cameras
.\build-live\Debug\rtsp_server.exe --list-microphones
.\build-live\Debug\rtsp_server.exe --camera 0 --microphone 0
~~~

另开一个终端播放：

~~~powershell
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
~~~

- 缺设备、编号不对或设备打不开时会报错，不回退到单轨。运行时任一路采集停止或失败，会停止两路并结束当前直播；请重启服务器重新采集。
- 摄像头默认请求 1280×720、30 fps、`yuyv422`。如果设备不支持，改成它支持的组合，例如加 `--width 640 --height 480 --fps 30`；用 `--help` 查看参数。
- 默认监听本机；按 Ctrl+C 关闭。不支持 UDP、录制或拖动进度条。

## 按这条路读代码

| 顺序 | 文件 | 只看这件事 |
| --- | --- | --- |
| 1 | [server_main.cpp](src/app/server_main.cpp) | `wmain()` 选两台设备，创建共同的时间起点 T0，再启动服务器。 |
| 2 | [camera_video_source.cpp](src/media/camera_video_source.cpp) / [microphone_audio_source.cpp](src/media/microphone_audio_source.cpp) | 各自的 `produce()` 在线程里采集和编码，将完整编码帧放进有界队列。 |
| 3 | [media_session.cpp](src/media/media_session.cpp) | `tick()` 每路先取一帧，比较处在同一时间线上的时间戳，把到期的数据打包、分发。 |
| 4 | [rtsp_server_connection.cpp](src/rtsp/rtsp_server_connection.cpp) | `DESCRIBE` 告诉播放器有哪两轨，`SETUP` 协商传输通道，`PLAY` 后在同一个 TCP 连接上发媒体。 |

先看入口到两个队列，不必从 Winsock 或编码器内部细节读起。采集端只运行一份，多位观众各有发送队列；中途加入的观众先等视频关键帧，再收到相应的音频。RTCP 把音视频各自的 RTP 时钟关联到共同时间，供播放器同步。

## 测试

~~~powershell
ctest --test-dir build-live -C Debug --output-on-failure
~~~

默认测试不打开实体设备。测试内的合成帧经过真实 H.264/AAC 编码和 RTSP/TCP 发送；如果找到 Python 和 `ffmpeg.exe`，还会注册 `live_av_integration` 验证两位客户端都能解码画面和音频。实体设备的画面、声音及同步仍要用上面的运行步骤确认。

根目录其他实现笔记记录历史版本；其中提到的文件模式、客户端、探针和旧 CMake 参数不适用于当前版本。
