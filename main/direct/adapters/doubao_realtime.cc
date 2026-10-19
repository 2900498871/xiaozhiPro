#include "doubao_realtime.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <web_socket.h>
#include <cJSON.h>

#include "board.h"

#define TAG "DoubaoAdapter"

// 简易 Base64 编码（与千问 adapter 共用实现，避免跨目录依赖）
static const char* B64_TABLE2 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string Base64Encode2(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out.push_back(B64_TABLE2[(n >> 18) & 0x3F]);
        out.push_back(B64_TABLE2[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? B64_TABLE2[(n >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? B64_TABLE2[n & 0x3F] : '=');
    }
    return out;
}

DoubaoRealtimeAdapter::DoubaoRealtimeAdapter() = default;
DoubaoRealtimeAdapter::~DoubaoRealtimeAdapter() { Close(); }

bool DoubaoRealtimeAdapter::Connect(const VoiceAdapterConfig& cfg) {
    cfg_ = cfg;
    if (cfg_.api_key.empty()) {
        if (on_error) on_error("API Key 为空");
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    ws_ = network->CreateWebSocket(1);
    if (!ws_) {
        if (on_error) on_error("创建 WebSocket 失败");
        return false;
    }

    // 端点默认值：以火山引擎官方文档为准（openspeech.bytedance.com 实时语音服务）
    std::string url = cfg_.base_url;
    if (url.empty()) {
        url = "wss://openspeech.bytedance.com/api/v3/realtime_voice?model=" + cfg_.model;
    }

    ws_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            // 下行音频二进制帧：pcm / ogg_opus（按会话配置）；解码后上抛
            if (on_audio) on_audio((const uint8_t*)data, len, 16000);
            return;
        }
        cJSON* root = cJSON_ParseWithLength(data, len);
        if (!root) return;
        // 火山协议：JSON 帧含 status/msg/result（以官方文档为准，TODO 完善事件解析）
        const char* status = cJSON_GetStringValue(cJSON_GetObjectItem(root, "status"));
        if (status) {
            if (strcmp(status, "ok") == 0) {
                if (on_state) on_state("streaming");
            } else if (strcmp(status, "error") == 0) {
                if (on_error) on_error("服务端错误");
            }
        }
        cJSON_Delete(root);
    });

    ws_->OnDisconnected([this]() {
        connected_ = false;
        if (on_state) on_state("closed");
    });

    if (!ws_->Connect(url.c_str())) {
        if (on_error) on_error("连接豆包失败");
        ws_.reset();
        return false;
    }
    connected_ = true;
    if (on_state) on_state("connecting");

    // 会话初始化：req_params（火山协议；字段以官方文档为准）
    cJSON* req = cJSON_CreateObject();
    cJSON* user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "uid", user_uid_.empty() ? "xiaozhi" : user_uid_.c_str());
    cJSON_AddItemToObject(req, "user", user);
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "api_key", cfg_.api_key.c_str());
    cJSON_AddStringToObject(params, "model", cfg_.model.c_str());
    cJSON* audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddNumberToObject(audio, "bits", 16);
    cJSON_AddNumberToObject(audio, "rate", 16000);
    cJSON_AddNumberToObject(audio, "channel", 1);
    cJSON_AddNumberToObject(audio, "sample_interval_ms", 20);
    cJSON_AddItemToObject(params, "audio", audio);
    cJSON* tts = cJSON_CreateObject();
    if (!cfg_.voice.empty()) cJSON_AddStringToObject(tts, "voice_type", cfg_.voice.c_str());
    cJSON_AddItemToObject(params, "tts", tts);
    cJSON_AddItemToObject(req, "req_params", params);
    cJSON_AddStringToObject(req, "audio", "");  // 首帧占位
    char* str = cJSON_PrintUnformatted(req);
    if (str) {
        ws_->Send(str);
        free(str);
    }
    cJSON_Delete(req);
    return true;
}

void DoubaoRealtimeAdapter::Close() {
    connected_ = false;
    if (ws_) {
        ws_->Close();
        ws_.reset();
    }
}

bool DoubaoRealtimeAdapter::SendAudio(const uint8_t* data, size_t len, bool is_opus) {
    if (!ws_ || !connected_.load() || !data || len == 0) return false;
    if (is_opus) {
        // 本适配器按 PCM16 上行；固件需通过 AudioService::SetSendPcm(true)。
        return false;
    }
    // 火山协议要求 20ms 对齐：PCM16@16k 单声道 = 640 字节/包。
    constexpr size_t kChunkBytes = 640;
    for (size_t offset = 0; offset < len; offset += kChunkBytes) {
        size_t chunk = len - offset;
        if (chunk > kChunkBytes) chunk = kChunkBytes;
        std::string b64 = Base64Encode2(data + offset, chunk);
        cJSON* msg = cJSON_CreateObject();
        cJSON* user = cJSON_CreateObject();
        cJSON_AddStringToObject(user, "uid", user_uid_.empty() ? "xiaozhi" : user_uid_.c_str());
        cJSON_AddItemToObject(msg, "user", user);
        cJSON_AddStringToObject(msg, "audio", b64.c_str());
        char* str = cJSON_PrintUnformatted(msg);
        bool ok = false;
        if (str) {
            ok = ws_->Send(str);
            free(str);
        }
        cJSON_Delete(msg);
        if (!ok) return false;
    }
    return true;
}

void DoubaoRealtimeAdapter::Interrupt() {
    if (!ws_ || !connected_) return;
    // 全双工原生打断：发送 interrupt 事件（字段以官方文档为准，TODO）
}

bool DoubaoRealtimeAdapter::IsConnected() const { return connected_; }
