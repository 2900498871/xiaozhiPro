#pragma once

#include <cstdint>
#include <functional>
#include <string>

// 语音直连服务商统一适配接口
// 新增服务商只需实现本接口（如 deepseek 预留），上层 DirectVoiceProtocol 无需改动

struct VoiceAdapterConfig {
    std::string provider;        // qwen / doubao / deepseek
    std::string api_key;
    std::string model;
    std::string base_url;
    std::string audio_format;    // 输出音频格式 wav / pcm / ogg_opus
    std::string voice;
    std::string language;
    bool interrupt_enable = true;
    int session_timeout_s = 60;
};

class VoiceAdapter {
public:
    virtual ~VoiceAdapter() = default;

    // 回调（由 DirectVoiceProtocol 注册）
    std::function<void(const uint8_t* pcm, size_t len, int sample_rate)> on_audio;   // 下行音频
    std::function<void(const std::string& text)> on_text;                            // 下行文本（助手回复）
    std::function<void(const std::string& text)> on_user_text;                       // 上行用户识别文本
    std::function<void(const std::string& state)> on_state;                          // connecting/streaming/closed
    std::function<void(const std::string& error)> on_error;                          // 错误描述

    virtual bool Connect(const VoiceAdapterConfig& cfg) = 0;
    virtual void Close() = 0;

    // 上行音频：is_opus=true 时 data 为 Opus 帧，false 时为原始 PCM16 单声道
    virtual bool SendAudio(const uint8_t* data, size_t len, bool is_opus) = 0;

    // 上行图像（JPEG），视觉能力（千问支持；豆包待官方确认）
    virtual bool SendImage(const uint8_t* jpeg, size_t len) { (void)jpeg; (void)len; return false; }

    // 打断当前输出
    virtual void Interrupt() {}

    // 提交已缓冲的上行输入并触发一次模型响应（StopListening 后调用；无缓冲语义的服务商可不实现）
    virtual void CommitInput() {}

    // 已发送图像后请求模型给出一次回复（视觉分析）
    virtual void RequestResponse() {}

    virtual bool IsConnected() const = 0;

    // 是否原生全双工（可边听边说）
    virtual bool IsFullDuplex() const { return false; }
};
