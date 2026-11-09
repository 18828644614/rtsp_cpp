#pragma once

#include "media/camera_capture.h"
#include "media/camera_video_source.h"

namespace rtsp {

// 仅同时开启 ENABLE_CAMERA_CAPTURE/ENABLE_H264_ENCODING 时提供。
// 工厂只保存配置；设备打开、读取和关闭均由 CameraVideoSource 工作线程执行。
std::unique_ptr<CameraFrameInput> make_camera_capture_input(const CameraConfig& config);

}  // namespace rtsp
