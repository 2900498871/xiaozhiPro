#include "qwen_realtime.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include <esp_log.h>
#include <web_socket.h>

#include "board.h"

#define TAG "QwenAdapter"

#ifndef QWEN_DEFAULT_BASE_URL
#define QWEN_DEFAULT_BASE_URL "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"
#endif
#ifndef QWEN_DEFAULT_MODEL
#define QWEN_DEFAULT_MODEL "qwen-omni-turbo-realtime"
#endif

// ---- 简易 Base64（IDF 无内置通用 API 时用） ----
static const char* B64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string Base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out.push_back(B64_TABLE[(n >> 18) & 0x3F]);
        out.push_back(B64_TABLE[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? B64_TABLE[(n >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? B64_TABLE[n & 0x3F] : '=');
    }
    return out;
}

static int B64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// 返回解码后的字节数（失败返回 -1）
static int Base64Decode(const char* in, size_t in_len, std::vector<uint8_t>& out) {
    out.clear();
    if (in_len % 4 != 0) return -1;
    out.reserve(in_len / 4 * 3);
    for (size_t i = 0; i < in_len; i += 4) {
        int a = B64Val(in[i]), b = B64Val(in[i + 1]);
        int c = in[i + 2] == '=' ? 0 : B64Val(in[i + 2]);
        int d = in[i + 3] == '=' ? 0 : B64Val(in[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
        uint32_t n = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
        out.push_back((n >> 16) & 0xFF);
        if (in[i + 2] != '=') out.push_back((n >> 8) & 0xFF);
        if (in[i + 3] != '=') out.push_back(n & 0xFF);
    }
    return (int)out.size();
}

QwenRealtimeAdapter::QwenRealtimeAdapter() = default;
QwenRealtimeAdapter::~QwenRealtimeAdapter() { Close(); }

bool QwenRealtimeAdapter::SendJson(cJSON* root) {
    if (!ws_ || !connected_.load()) return false;
    char* str = cJSON_PrintUnformatted(root);
    if (str == nullptr) return false;
    std::string payload(str);
    free(str);

    std::lock_guard<std::mutex> lock(send_mutex_);
    // WebSocket 实现单帧上限 65535 字节，超大 JSON（含 Base64 图像）需按
    // RFC 6455 分片；分片期间持有 send_mutex_，避免插入音频帧。
    constexpr size_t kMaxFrame = 60000;
    if (payload.size() <= kMaxFrame) {
        return ws_->Send(payload);
    }
    size_t offset = 0;
    while (offset < payload.size()) {
        size_t chunk = payload.size() - offset;
        if (chunk > kMaxFrame) chunk = kMaxFrame;
        const bool fin = (offset + chunk == payload.size());
        if (!ws_->Send(payload.data() + offset, chunk, false, fin)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

void QwenRealtimeAdapter::SendSessionUpdate() {
    cJSON* session = cJSON_CreateObject();
    cJSON_AddStringToObject(session, "type", "session.update");
    cJSON* s = cJSON_CreateObject();
    cJSON* modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(s, "modalities", modalities);
    cJSON_AddStringToObject(s, "voice", cfg_.voice.empty() ? "Chelsie" : cfg_.voice.c_str());
    cJSON_AddStringToObject(s, "instructions",
                            "你是小智，一个友好的中文语音助手。回答简洁、口语化。");
    cJSON_AddStringToObject(s, "input_audio_format", "pcm16");
    cJSON_AddStringToObject(s, "output_audio_format", "pcm24");
    // 手动模式：由固件在 StopListening 时提交并触发回复，避免服务端 VAD 重复响应。
    cJSON_AddItemToObject(s, "turn_detection", cJSON_CreateNull());
    // 原始 WebSocket 需显式开启输入转写，否则收不到用户语音文本，
    // 语音指令（如"打开摄像头"）也无法生效。
    cJSON* transcription = cJSON_CreateObject();
    cJSON_AddStringToObject(transcription, "model", "qwen3-asr-flash-realtime");
    cJSON_AddItemToObject(s, "input_audio_transcription", transcription);
    cJSON_AddItemToObject(session, "session", s);
    SendJson(session);
    cJSON_Delete(session);
}

bool QwenRealtimeAdapter::Connect(const VoiceAdapterConfig& cfg) {
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

    std::string base_url = cfg_.base_url.empty() ? QWEN_DEFAULT_BASE_URL : cfg_.base_url;
    std::string model = cfg_.model.empty() ? QWEN_DEFAULT_MODEL : cfg_.model;
    std::string url = base_url;
    url += (url.find('?') == std::string::npos) ? "?model=" : "&model=";
    url += model;

    std::string auth = "Bearer " + cfg_.api_key;
    ws_->SetHeader("Authorization", auth.c_str());
    ws_->SetHeader("OpenAI-Beta", "realtime=v1");

    ws_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            // 若服务端改用二进制下发，按 pcm24 处理（防御性兼容）。
            if (on_audio) on_audio(reinterpret_cast<const uint8_t*>(data), len, 24000);
            return;
        }
        cJSON* root = cJSON_ParseWithLength(data, len);
        if (!root) return;
        const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
        if (!type) {
            cJSON_Delete(root);
            return;
        }
        if (strcmp(type, "session.created") == 0 || strcmp(type, "session.updated") == 0) {
            connected_ = true;
            if (on_state) on_state("streaming");
        } else if (strcmp(type, "response.audio.delta") == 0 ||
                   strcmp(type, "response.output_audio.delta") == 0) {
            const char* delta = cJSON_GetStringValue(cJSON_GetObjectItem(root, "delta"));
            if (delta && on_audio) {
                std::vector<uint8_t> pcm;
                if (Base64Decode(delta, strlen(delta), pcm) > 0) {
                    // output_audio_format=pcm24
                    on_audio(pcm.data(), pcm.size(), 24000);
                }
            }
        } else if (strcmp(type, "response.audio_transcript.delta") == 0 ||
                   strcmp(type, "response.output_audio_transcript.delta") == 0 ||
                   strcmp(type, "response.text.delta") == 0 ||
                   strcmp(type, "response.output_text.delta") == 0) {
            const char* text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "delta"));
            if (text && on_text) on_text(text);
        } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
            const char* text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "transcript"));
            if (text && on_user_text) on_user_text(text);
        } else if (strcmp(type, "conversation.item.input_audio_transcription.failed") == 0) {
            cJSON* err = cJSON_GetObjectItem(root, "error");
            const char* msg =
                err ? cJSON_GetStringValue(cJSON_GetObjectItem(err, "message")) : nullptr;
            ESP_LOGW(TAG, "input transcription failed: %s", msg ? msg : "unknown");
        } else if (strcmp(type, "response.audio.done") == 0) {
            if (on_state) on_state("audio_done");
        } else if (strcmp(type, "response.done") == 0) {
            if (on_state) on_state("response_done");
        } else if (strcmp(type, "error") == 0) {
            cJSON* err = cJSON_GetObjectItem(root, "error");
            const char* msg = err ? cJSON_GetStringValue(cJSON_GetObjectItem(err, "message")) : nullptr;
            if (on_error) on_error(msg ? msg : "服务端错误");
        }
        cJSON_Delete(root);
    });

    ws_->OnDisconnected([this]() {
        connected_ = false;
        if (on_state) on_state("closed");
    });

    if (!ws_->Connect(url.c_str())) {
        if (on_error) on_error("连接千问 Realtime 失败");
        ws_.reset();
        return false;
    }
    // 部分 WebSocket 实现同步返回时服务端可能已推送 session.created；
    // 这里仍先标记已连接，确保 session.update 能发出。
    connected_ = true;
    sent_audio_ = false;
    if (on_state) on_state("connecting");
    SendSessionUpdate();
    return true;
}

void QwenRealtimeAdapter::Close() {
    connected_ = false;
    sent_audio_ = false;
    if (ws_) {
        ws_->Close();
        ws_.reset();
    }
}

bool QwenRealtimeAdapter::SendAudio(const uint8_t* data, size_t len, bool is_opus) {
    if (!ws_ || !connected_.load() || !data || len == 0) return false;
    if (is_opus) {
        // input_audio_format 仅支持 pcm16；固件应通过 AudioService::SetSendPcm(true) 上行。
        ESP_LOGW(TAG, "opus input is not supported by DashScope, enable PCM sending");
        return false;
    }
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "input_audio_buffer.append");
    std::string b64 = Base64Encode(data, len);
    cJSON_AddStringToObject(msg, "audio", b64.c_str());
    bool ok = SendJson(msg);
    cJSON_Delete(msg);
    if (ok) sent_audio_ = true;
    return ok;
}

bool QwenRealtimeAdapter::SendImage(const uint8_t* jpeg, size_t len) {
    if (!ws_ || !connected_.load() || !jpeg || len == 0) return false;
    // Base64 膨胀 4/3，单帧上限 60KB，过大图像会被 WebSocket 拒绝。
    if (len > 64 * 1024) {
        if (on_error) on_error("图像过大（建议 JPEG < 64KB，可降低分辨率或质量）");
        return false;
    }
    // DashScope 要求图像与音频缓冲一起提交；先补 100ms 静音，
    // 保证随后 input_audio_buffer.commit 不会因缓冲为空而失败。
    std::vector<uint8_t> silence(16000 / 10 * sizeof(int16_t), 0);  // 100ms 静音
    SendAudio(silence.data(), silence.size(), false);
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "input_image_buffer.append");
    std::string b64 = Base64Encode(jpeg, len);
    cJSON_AddStringToObject(msg, "image", b64.c_str());
    bool ok = SendJson(msg);
    cJSON_Delete(msg);
    return ok;
}

void QwenRealtimeAdapter::Interrupt() {
    if (!ws_ || !connected_.load()) return;
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "response.cancel");
    SendJson(msg);
    cJSON_Delete(msg);
}

void QwenRealtimeAdapter::CommitInput() {
    if (!ws_ || !connected_.load()) return;
    cJSON* commit = cJSON_CreateObject();
    cJSON_AddStringToObject(commit, "type", "input_audio_buffer.commit");
    SendJson(commit);
    cJSON_Delete(commit);
    SendResponseCreate();
}

void QwenRealtimeAdapter::RequestResponse() {
    if (!ws_ || !connected_.load()) return;
    // 视觉输入：图像与音频缓冲一并提交后再请求回复。
    cJSON* commit = cJSON_CreateObject();
    cJSON_AddStringToObject(commit, "type", "input_audio_buffer.commit");
    SendJson(commit);
    cJSON_Delete(commit);
    SendResponseCreate();
}

void QwenRealtimeAdapter::SendResponseCreate() {
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response.create");
    cJSON* r = cJSON_CreateObject();
    cJSON* modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(r, "modalities", modalities);
    cJSON_AddItemToObject(resp, "response", r);
    SendJson(resp);
    cJSON_Delete(resp);
}

bool QwenRealtimeAdapter::IsConnected() const { return connected_.load(); }
