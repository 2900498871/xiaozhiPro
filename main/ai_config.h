#pragma once

#include <string>

// AI 直连模式配置（NVS 命名空间 llm / vision）
// 由板载配置网页读写，固件直连协议层读取

struct LlmConfig {
    std::string provider;        // qwen / doubao / deepseek(预留)
    std::string api_key;         // DashScope / 火山 Key（页面不回显）
    std::string model;           // qwen-omni-turbo-realtime / doubao-realtime-3.0
    std::string base_url;        // 接入端点（带默认值，可改）
    std::string audio_format;    // pcm / wav / ogg_opus（输出格式，当前 Realtime 直连固定 PCM）
    std::string voice;           // 音色
    std::string language;        // 语言/方言
    bool interrupt_enable = true;
    int session_timeout_s = 60;  // 会话空闲超时（秒）
};

struct VisionConfig {
    bool enable = false;             // 摄像头开机自启
    int width = 640;
    int height = 480;
    int quality = 60;                // JPEG 质量 10~100
    std::string trigger_mode = "both";  // voice / manual / both
    std::string switch_phrases = "打开摄像头,关闭摄像头";
    std::string command_phrases = "看看,这是什么,帮我看看";
    bool continuous_enable = true;   // 打开后连续帧分析
    int frame_interval_ms = 2000;    // 连续分析帧间隔
    std::string announce_mode = "quiet";  // quiet / periodic / event
    int auto_off_ms = 0;              // 自动关闭毫秒，0=不自动关
};

// 读取（带默认值）
LlmConfig LlmConfigLoad();
VisionConfig VisionConfigLoad();

// 保存
void LlmConfigSave(const LlmConfig& cfg);
void VisionConfigSave(const VisionConfig& cfg);

// 设备名称（NVS 命名空间 device / 键 name，需求 FR-2.2）
std::string DeviceNameLoad();
void DeviceNameSave(const std::string& name);
