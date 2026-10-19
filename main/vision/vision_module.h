#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

// OV2640 摄像头模块：生命周期、语音开关、连续帧分析
// 接线（DVP 8bit + SCCB）。默认引脚取自 bread-compact-wifi (YD-ESP32-S3 N16R8)
// 的空闲 GPIO；避开了 Octal PSRAM (GPIO33-37)、USB (19/20)、UART (43/44) 及板载外设。
// 若实际接线不同，只需修改以下宏并重新编译。

// ---- 摄像头引脚分配 ----
// 注意：摄像头模块的 PWDN 直接接地（-1），由 esp_camera_deinit() 停止时钟省电。
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1   // 未接（NC）
#define CAM_PIN_XCLK  13
#define CAM_PIN_SIOD  38   // SCCB data
#define CAM_PIN_SIOC  45   // SCCB clock
#define CAM_PIN_D7    12
#define CAM_PIN_D6    11
#define CAM_PIN_D5    10
#define CAM_PIN_D4    9
#define CAM_PIN_D3    8
#define CAM_PIN_D2    3
#define CAM_PIN_D1    2
#define CAM_PIN_D0    1
#define CAM_PIN_VSYNC 17
#define CAM_PIN_HREF  21
#define CAM_PIN_PCLK  14
#define CAM_XCLK_FREQ 20000000

// 连续帧分析回调：jpeg 帧数据（有效期内使用，任务内同步调用）
typedef std::function<void(const uint8_t* jpeg, size_t len)> VisionFrameCallback;

// 启动摄像头并进入连续帧分析（按 vision/* 配置）
bool VisionModuleStart();
// 停止分析并下电（省电）
void VisionModuleStop();
// 取一帧 JPEG（malloc，调用方 free；失败返回 nullptr）
uint8_t* VisionModuleSnapshotJpeg(size_t* len);
// 运行状态
bool VisionModuleIsRunning();
// 注册连续帧回调（每 frame_interval_ms 触发）
void VisionModuleSetFrameCallback(VisionFrameCallback cb);
// 网页预览：返回最近一帧 JPEG（malloc，调用方 free）
uint8_t* VisionModulePreviewJpeg(size_t* len);
