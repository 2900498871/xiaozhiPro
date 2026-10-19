#pragma once

#include "adapter_interface.h"

// DeepSeek 预留适配器（占位）
// DeepSeek 目前仅文本 API，无语音/视觉能力；待官方开放后按 AdapterInterface 填充
class DeepSeekAdapter : public VoiceAdapter {
public:
    bool Connect(const VoiceAdapterConfig& cfg) override;
    void Close() override;
    bool SendAudio(const uint8_t* data, size_t len, bool is_opus) override;
    bool IsConnected() const override;
};
