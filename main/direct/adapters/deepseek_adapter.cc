#include "deepseek_adapter.h"

#include <esp_log.h>

#define TAG "DeepSeekAdapter"

bool DeepSeekAdapter::Connect(const VoiceAdapterConfig& cfg) {
    (void)cfg;
    if (on_error) on_error("DeepSeek 语音 API 尚未开放，当前仅支持千问与豆包");
    if (on_state) on_state("closed");
    return false;
}

void DeepSeekAdapter::Close() {}

bool DeepSeekAdapter::SendAudio(const uint8_t* data, size_t len, bool is_opus) {
    (void)data;
    (void)len;
    (void)is_opus;
    return false;
}

bool DeepSeekAdapter::IsConnected() const { return false; }
