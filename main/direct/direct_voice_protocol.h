#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "protocols/protocol.h"
#include "ai_config.h"

class VoiceAdapter;

// 纯直连语音协议层：不连接 xiaozhi 服务器，直接经 AdapterInterface 调用
// 千问 / 豆包 / DeepSeek(预留) 官方 API。替代 MQTT/WebSocket 服务器模式。
class DirectVoiceProtocol : public Protocol {
public:
    DirectVoiceProtocol();
    ~DirectVoiceProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendWakeWordDetected(const std::string& wake_word) override;

    // ---- 视觉扩展（网页 / 语音指令调用） ----
    // 语音开关摄像头：text 为识别出的用户指令文本，命中 switch_phrases 时执行开/关
    bool HandleVisionCommand(const std::string& text);
    // 触发一次拍照识别（adapter 支持图像时发送最近帧）
    bool SnapAndAnalyze();

    // 供 config_server 查询
    const std::string& provider() const { return provider_; }
    bool IsVisionRunning() const;
    // 会话空闲超时（llm/session_timeout）到期，由主循环轮询后关闭会话。
    bool IsIdleTimedOut() const;

private:
    bool SendText(const std::string& text) override;

    void CreateAdapter();
    void OnAdapterAudio(const uint8_t* pcm, size_t len, int sample_rate);
    void OnAdapterState(const std::string& state);
    void OnAdapterText(const std::string& text);
    void OnUserText(const std::string& text);
    void OnVisionFrame(const uint8_t* jpeg, size_t len);
    void EmitTts(const char* state, const std::string& text = "");

    std::unique_ptr<VoiceAdapter> adapter_;
    std::string provider_;
    LlmConfig cfg_;
    bool started_ = false;

    // 一次模型回复的生命周期（适配器回调线程与主任务都会访问）
    std::mutex state_mutex_;
    bool response_active_ = false;
    bool tts_started_ = false;
    std::string response_text_;
    // 已发送图像 / 已提交输入，等待模型回复；期间不再注入新图像
    std::atomic<bool> request_in_flight_{false};
    // 正在采集用户语音；此时提交图像缓冲会截断当前这轮输入。
    std::atomic<bool> listening_{false};
    // quiet 模式：缓存最近一帧，用户提问提交时随音频一起发送（受 state_mutex_ 保护）
    std::vector<uint8_t> quiet_frame_;
    int64_t quiet_frame_us_ = 0;
    std::atomic<int64_t> last_activity_us_{0};
};
