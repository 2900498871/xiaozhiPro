#pragma once

#include "adapter_interface.h"
#include <atomic>
#include <cJSON.h>
#include <memory>
#include <mutex>

class WebSocket;

// 阿里千问 Qwen-Omni —— DashScope Realtime API
// 文档：wss://dashscope.aliyuncs.com/api-ws/v1/realtime
// 输入音频：pcm16 @16kHz；输出音频：pcm24 @24kHz。
// 图像：input_image_buffer.append（需先发送过音频）。
class QwenRealtimeAdapter : public VoiceAdapter {
public:
    QwenRealtimeAdapter();
    ~QwenRealtimeAdapter() override;

    bool Connect(const VoiceAdapterConfig& cfg) override;
    void Close() override;
    bool SendAudio(const uint8_t* data, size_t len, bool is_opus) override;
    bool SendImage(const uint8_t* jpeg, size_t len) override;
    void Interrupt() override;
    void CommitInput() override;
    void RequestResponse() override;
    bool IsConnected() const override;
    bool IsFullDuplex() const override { return false; }

private:
    bool SendJson(cJSON* root);
    void SendSessionUpdate();
    void SendResponseCreate();

    std::shared_ptr<WebSocket> ws_;
    VoiceAdapterConfig cfg_;
    std::atomic<bool> connected_{false};
    // DashScope requires at least one audio append before an image append.
    bool sent_audio_ = false;
    // WebSocket 单帧上限 65535 字节；图像消息需分片发送，且分片之间不能插入
    // 其他任务的音频消息，因此所有发送必须串行化。
    std::mutex send_mutex_;
};
