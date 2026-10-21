#include "ai_config.h"
#include "settings.h"

#include <cstring>

#include <esp_log.h>

#define TAG "AiConfig"

static const char* DEFAULT_LLM_BASE_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime";

// 旧版本（v1.3 早期）写入的端点/模型已失效，读取时自动迁移一次。
static void MigrateQwenDefaults(LlmConfig& cfg) {
    if (cfg.provider != "qwen") return;
    bool migrated = false;
    if (cfg.model == "qwen2.5-omni") {
        cfg.model = "qwen-omni-turbo-realtime";
        migrated = true;
    }
    size_t pos = cfg.base_url.find("/api/v1/realtime");
    if (pos != std::string::npos) {
        cfg.base_url.replace(pos, strlen("/api/v1/realtime"), "/api-ws/v1/realtime");
        migrated = true;
    }
    if (!migrated) return;
    Settings s("llm", true);
    s.SetString("model", cfg.model);
    s.SetString("base_url", cfg.base_url);
    ESP_LOGI(TAG, "Migrated qwen defaults: model=%s url=%s", cfg.model.c_str(),
             cfg.base_url.c_str());
}

LlmConfig LlmConfigLoad() {
    LlmConfig cfg;
    Settings s("llm", false);
    cfg.provider = s.GetString("provider", "qwen");
    cfg.api_key = s.GetString("api_key", "");
    cfg.model = s.GetString("model", "qwen-omni-turbo-realtime");
    cfg.base_url = s.GetString("base_url", DEFAULT_LLM_BASE_URL);
    cfg.audio_format = s.GetString("audio_format", "pcm");
    cfg.voice = s.GetString("voice", "Chelsie");
    cfg.language = s.GetString("language", "zh");
    cfg.interrupt_enable = s.GetBool("interrupt_enable", true);
    cfg.session_timeout_s = s.GetInt("session_timeout", 60);
    MigrateQwenDefaults(cfg);
    return cfg;
}

void LlmConfigSave(const LlmConfig& cfg) {
    Settings s("llm", true);
    s.SetString("provider", cfg.provider);
    if (!cfg.api_key.empty()) {
        s.SetString("api_key", cfg.api_key);
    }
    s.SetString("model", cfg.model);
    s.SetString("base_url", cfg.base_url);
    s.SetString("audio_format", cfg.audio_format);
    s.SetString("voice", cfg.voice);
    s.SetString("language", cfg.language);
    s.SetBool("interrupt_enable", cfg.interrupt_enable);
    s.SetInt("session_timeout", cfg.session_timeout_s);
    ESP_LOGI(TAG, "Llm config saved: provider=%s model=%s", cfg.provider.c_str(), cfg.model.c_str());
}

VisionConfig VisionConfigLoad() {
    VisionConfig cfg;
    Settings s("vision", false);
    cfg.enable = s.GetBool("enable", false);
    cfg.width = s.GetInt("width", 640);
    cfg.height = s.GetInt("height", 480);
    cfg.quality = s.GetInt("quality", 60);
    cfg.trigger_mode = s.GetString("trigger_mode", "both");
    cfg.switch_phrases = s.GetString("switch_phrases", "打开摄像头,关闭摄像头");
    cfg.command_phrases = s.GetString("command_phrases", "看看,这是什么,帮我看看");
    cfg.continuous_enable = s.GetBool("continuous_enable", true);
    cfg.frame_interval_ms = s.GetInt("frame_interval_ms", 2000);
    cfg.announce_mode = s.GetString("announce_mode", "quiet");
    cfg.auto_off_ms = s.GetInt("auto_off_ms", 0);
    return cfg;
}

std::string DeviceNameLoad() {
    Settings s("device", false);
    return s.GetString("name", "");
}

void DeviceNameSave(const std::string& name) {
    Settings s("device", true);
    if (name.empty()) {
        s.EraseKey("name");
    } else {
        s.SetString("name", name);
    }
    ESP_LOGI(TAG, "Device name saved: %s", name.c_str());
}

void VisionConfigSave(const VisionConfig& cfg) {
    Settings s("vision", true);
    s.SetBool("enable", cfg.enable);
    s.SetInt("width", cfg.width);
    s.SetInt("height", cfg.height);
    s.SetInt("quality", cfg.quality);
    s.SetString("trigger_mode", cfg.trigger_mode);
    s.SetString("switch_phrases", cfg.switch_phrases);
    s.SetString("command_phrases", cfg.command_phrases);
    s.SetBool("continuous_enable", cfg.continuous_enable);
    s.SetInt("frame_interval_ms", cfg.frame_interval_ms);
    s.SetString("announce_mode", cfg.announce_mode);
    s.SetInt("auto_off_ms", cfg.auto_off_ms);
    ESP_LOGI(TAG, "Vision config saved");
}
