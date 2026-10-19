#pragma once

#include "adapter_interface.h"
#include <atomic>
#include <memory>

class WebSocket;

// 字节豆包 —— 端到端实时语音（火山引擎 WebSocket）
// 输入：PCM16（16k 单声道）；按 20ms/640B 分包 Base64；输出：pcm / ogg_opus / mp3
// 注意：火山实时语音的二进制事件帧（header/event/payload）尚未完整实现，
// 当前仅完成连接与 PCM 上行分包；下行事件解析需按官方文档补齐。
class DoubaoRealtimeAdapter : public VoiceAdapter {
public:
    DoubaoRealtimeAdapter();
    ~DoubaoRealtimeAdapter() override;

    bool Connect(const VoiceAdapterConfig& cfg) override;
    void Close() override;
    bool SendAudio(const uint8_t* data, size_t len, bool is_opus) override;
    void Interrupt() override;
    bool IsConnected() const override;
    bool IsFullDuplex() const override { return true; }  // Seeduplex 全双工

private:
    std::shared_ptr<WebSocket> ws_;
    VoiceAdapterConfig cfg_;
    std::atomic<bool> connected_{false};
    std::string user_uid_;
};
