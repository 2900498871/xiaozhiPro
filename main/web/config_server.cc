#include "config_server.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_http_client.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>

#include "ai_config.h"
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "direct/direct_voice_protocol.h"
#include "display.h"
#include "display/ip_hint.h"
#include "settings.h"
#include "vision/vision_module.h"

#define TAG "ConfigServer"

static httpd_handle_t s_server = nullptr;
static DirectVoiceProtocol* s_protocol = nullptr;

// ---------------- 工具 ----------------

static std::string GetDeviceIp() {
    esp_netif_ip_info_t ip_info;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return "0.0.0.0";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
    return buf;
}

static std::string GetFwVersion() {
    const esp_app_desc_t* desc = esp_app_get_description();
    return desc != nullptr ? std::string(desc->version) : std::string("unknown");
}

static std::string GetSsid() {
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return std::string(reinterpret_cast<const char*>(ap.ssid));
    }
    return "";
}

static std::string GetMac() {
    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char buf[24];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

static int JsonGetInt(cJSON* root, const char* key, int def) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (!item) return def;
    if (cJSON_IsNumber(item)) return item->valueint;
    if (cJSON_IsString(item) && item->valuestring) return atoi(item->valuestring);
    return def;
}

static bool JsonGetBool(cJSON* root, const char* key, bool def) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (!item) return def;
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valueint != 0;
    if (cJSON_IsString(item) && item->valuestring) {
        return strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0;
    }
    return def;
}

static std::string JsonGetString(cJSON* root, const char* key, const std::string& def) {
    cJSON* item = cJSON_GetObjectItem(root, key);
    if (!item) return def;
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return def;
}

static void SendJson(httpd_req_t* req, const char* json, const char* status = "200 OK") {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

// 完整读取请求体（httpd_req_recv 可能只返回部分数据）。
static bool ReadBody(httpd_req_t* req, std::string& out, size_t max_len = 8192) {
    out.clear();
    const size_t content_len = req->content_len;
    if (content_len == 0 || content_len > max_len) {
        return false;
    }
    out.resize(content_len);
    size_t received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, out.data() + received, content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return false;
        }
        received += (size_t)ret;
    }
    return true;
}

static int ClampInt(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

// 按 UTF-8 字符边界截断，避免中文名被切成半个字符后显示为乱码。
static std::string Utf8Truncate(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    size_t len = max_bytes;
    while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80) {
        --len;
    }
    return text.substr(0, len);
}

// 在屏幕上给出操作反馈（HTTP/语音任务不能直接操作显示，交给主任务）。
static void NotifyDisplay(const std::string& message) {
    if (message.empty()) return;
    Application::GetInstance().Schedule([message]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->ShowNotification(message.c_str(), 3000);
        }
    });
}

// ---------------- 串口日志镜像（FR-6.4） ----------------
// 固定环形缓冲，避免日志量随运行时间增长；读取时按时间顺序复制一份快照。
static constexpr size_t kLogRingSize = 8192;
static char s_log_ring[kLogRingSize];
static size_t s_log_head = 0;  // 下一个写入位置
static size_t s_log_len = 0;   // 有效字节数
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf = nullptr;

static int LogCapture(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int ret = s_prev_vprintf != nullptr ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);

    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n > 0) {
        const size_t len =
            static_cast<size_t>(n) < sizeof(buf) - 1 ? static_cast<size_t>(n) : sizeof(buf) - 1;
        portENTER_CRITICAL(&s_log_mux);
        for (size_t i = 0; i < len; ++i) {
            s_log_ring[s_log_head] = buf[i];
            s_log_head = (s_log_head + 1) % kLogRingSize;
        }
        s_log_len = s_log_len + len > kLogRingSize ? kLogRingSize : s_log_len + len;
        portEXIT_CRITICAL(&s_log_mux);
    }
    return ret;
}

static std::string LogRead() {
    // 先在临界区外完成 8KB 扩容，避免持有自旋锁时触发堆分配。
    std::string out;
    out.resize(kLogRingSize);
    size_t len = 0;
    portENTER_CRITICAL(&s_log_mux);
    len = s_log_len;
    const size_t start = (s_log_head + kLogRingSize - len) % kLogRingSize;
    for (size_t i = 0; i < len; ++i) {
        out[i] = s_log_ring[(start + i) % kLogRingSize];
    }
    portEXIT_CRITICAL(&s_log_mux);
    out.resize(len);
    return out;
}

static void LogClear() {
    portENTER_CRITICAL(&s_log_mux);
    s_log_head = 0;
    s_log_len = 0;
    portEXIT_CRITICAL(&s_log_mux);
}

static void LogHookInstall() {
    if (s_prev_vprintf == nullptr) {
        s_prev_vprintf = esp_log_set_vprintf(LogCapture);
    }
}

static void LogHookRestore() {
    if (s_prev_vprintf != nullptr) {
        esp_log_set_vprintf(s_prev_vprintf);
        s_prev_vprintf = nullptr;
    }
}

// ---------------- 配置序列化 ----------------

// export_mode=true 时输出可导入的完整配置（API Key 脱敏）；
// 否则输出网页使用的配置，Key 只返回是否已设置。
static cJSON* BuildConfigJson(bool export_mode) {
    LlmConfig lcfg = LlmConfigLoad();
    VisionConfig vcfg = VisionConfigLoad();
    cJSON* root = cJSON_CreateObject();
    cJSON* llm = cJSON_CreateObject();
    cJSON_AddStringToObject(llm, "provider", lcfg.provider.c_str());
    if (export_mode) {
        cJSON_AddStringToObject(llm, "api_key", lcfg.api_key.empty() ? "" : "********");
    } else {
        cJSON_AddBoolToObject(llm, "has_api_key", !lcfg.api_key.empty());
    }
    cJSON_AddStringToObject(llm, "model", lcfg.model.c_str());
    cJSON_AddStringToObject(llm, "base_url", lcfg.base_url.c_str());
    cJSON_AddStringToObject(llm, "audio_format", lcfg.audio_format.c_str());
    cJSON_AddStringToObject(llm, "voice", lcfg.voice.c_str());
    cJSON_AddStringToObject(llm, "language", lcfg.language.c_str());
    cJSON_AddBoolToObject(llm, "interrupt_enable", lcfg.interrupt_enable);
    cJSON_AddNumberToObject(llm, "session_timeout", lcfg.session_timeout_s);
    cJSON_AddItemToObject(root, "llm", llm);

    cJSON* vision = cJSON_CreateObject();
    cJSON_AddBoolToObject(vision, "enable", vcfg.enable);
    cJSON_AddNumberToObject(vision, "width", vcfg.width);
    cJSON_AddNumberToObject(vision, "height", vcfg.height);
    cJSON_AddNumberToObject(vision, "quality", vcfg.quality);
    cJSON_AddStringToObject(vision, "trigger_mode", vcfg.trigger_mode.c_str());
    cJSON_AddStringToObject(vision, "switch_phrases", vcfg.switch_phrases.c_str());
    cJSON_AddStringToObject(vision, "command_phrases", vcfg.command_phrases.c_str());
    cJSON_AddBoolToObject(vision, "continuous_enable", vcfg.continuous_enable);
    cJSON_AddNumberToObject(vision, "frame_interval_ms", vcfg.frame_interval_ms);
    cJSON_AddStringToObject(vision, "announce_mode", vcfg.announce_mode.c_str());
    cJSON_AddNumberToObject(vision, "auto_off_ms", vcfg.auto_off_ms);
    cJSON_AddItemToObject(root, "vision", vision);

    cJSON* device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "name", DeviceNameLoad().c_str());
    auto codec = Board::GetInstance().GetAudioCodec();
    cJSON_AddNumberToObject(device, "volume", codec != nullptr ? codec->output_volume() : 0);
    cJSON_AddItemToObject(root, "device", device);
    return root;
}

// 校验并写入 llm/vision/device 配置；成功返回 true，失败时 error 为提示文本。
static bool ApplyConfigJson(cJSON* root, std::string& error) {
    LlmConfig lcfg = LlmConfigLoad();
    cJSON* llm = cJSON_GetObjectItem(root, "llm");
    if (llm) {
        std::string provider = JsonGetString(llm, "provider", lcfg.provider);
        if (provider == "qwen" || provider == "doubao" || provider == "deepseek") {
            lcfg.provider = provider;
        }
        std::string base_url = JsonGetString(llm, "base_url", lcfg.base_url);
        if (!base_url.empty() && base_url.rfind("wss://", 0) != 0 &&
            base_url.rfind("ws://", 0) != 0) {
            error = "base_url must start with wss:// or ws://";
            return false;
        }
        lcfg.base_url = base_url;
        std::string key = JsonGetString(llm, "api_key", "");
        const bool clear_key = JsonGetBool(llm, "clear_api_key", false);
        if (clear_key) {
            lcfg.api_key.clear();
        } else if (!key.empty() && key != "********") {
            // 空值或脱敏占位符表示保留原 Key（页面/导出文件不回显）。
            lcfg.api_key = key;
        }
        lcfg.model = JsonGetString(llm, "model", lcfg.model);
        lcfg.audio_format = JsonGetString(llm, "audio_format", lcfg.audio_format);
        lcfg.voice = JsonGetString(llm, "voice", lcfg.voice);
        lcfg.language = JsonGetString(llm, "language", lcfg.language);
        lcfg.interrupt_enable = JsonGetBool(llm, "interrupt_enable", lcfg.interrupt_enable);
        lcfg.session_timeout_s =
            ClampInt(JsonGetInt(llm, "session_timeout", lcfg.session_timeout_s), 5, 600);
        LlmConfigSave(lcfg);
        if (clear_key) {
            Settings llm_settings("llm", true);
            llm_settings.EraseKey("api_key");
        }
    }

    cJSON* device = cJSON_GetObjectItem(root, "device");
    if (device) {
        std::string name = JsonGetString(device, "name", DeviceNameLoad());
        DeviceNameSave(Utf8Truncate(name, 32));
        cJSON* volume = cJSON_GetObjectItem(device, "volume");
        if (volume != nullptr) {
            auto codec = Board::GetInstance().GetAudioCodec();
            if (codec != nullptr) {
                const int current_volume = codec->output_volume();
                codec->SetOutputVolume(
                    ClampInt(JsonGetInt(device, "volume", current_volume), 0, 100));
            }
        }
    }

    VisionConfig vcfg = VisionConfigLoad();
    cJSON* vision = cJSON_GetObjectItem(root, "vision");
    bool camera_params_changed = false;
    if (vision) {
        const int old_width = vcfg.width;
        const int old_height = vcfg.height;
        const int old_quality = vcfg.quality;
        vcfg.enable = JsonGetBool(vision, "enable", vcfg.enable);
        vcfg.width = ClampInt(JsonGetInt(vision, "width", vcfg.width), 160, 1600);
        vcfg.height = ClampInt(JsonGetInt(vision, "height", vcfg.height), 120, 1200);
        vcfg.quality = ClampInt(JsonGetInt(vision, "quality", vcfg.quality), 10, 100);
        std::string trigger_mode = JsonGetString(vision, "trigger_mode", vcfg.trigger_mode);
        if (trigger_mode == "voice" || trigger_mode == "manual" || trigger_mode == "both") {
            vcfg.trigger_mode = trigger_mode;
        }
        vcfg.switch_phrases = JsonGetString(vision, "switch_phrases", vcfg.switch_phrases);
        vcfg.command_phrases = JsonGetString(vision, "command_phrases", vcfg.command_phrases);
        vcfg.continuous_enable = JsonGetBool(vision, "continuous_enable", vcfg.continuous_enable);
        vcfg.frame_interval_ms =
            ClampInt(JsonGetInt(vision, "frame_interval_ms", vcfg.frame_interval_ms), 500, 10000);
        std::string announce_mode = JsonGetString(vision, "announce_mode", vcfg.announce_mode);
        if (announce_mode == "quiet" || announce_mode == "periodic" || announce_mode == "event") {
            vcfg.announce_mode = announce_mode;
        }
        vcfg.auto_off_ms = ClampInt(JsonGetInt(vision, "auto_off_ms", vcfg.auto_off_ms), 0, 600000);
        camera_params_changed = (vcfg.width != old_width || vcfg.height != old_height ||
                                 vcfg.quality != old_quality);
        VisionConfigSave(vcfg);
    }

    // 立即应用摄像头开关；分辨率/质量变化需要重启采集。
    if (vcfg.enable) {
        if (!VisionModuleIsRunning()) {
            VisionModuleStart();
        } else if (camera_params_changed) {
            VisionModuleStop();
            VisionModuleStart();
        }
    } else if (VisionModuleIsRunning()) {
        VisionModuleStop();
    }
    return true;
}

// ---------------- API handlers ----------------

static esp_err_t HandleStatus(httpd_req_t* req) {
    IpHint::MarkConfigured();
    IpHint::Hide("config page accessed");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ip", GetDeviceIp().c_str());
    cJSON_AddStringToObject(root, "fw_version", GetFwVersion().c_str());
    cJSON_AddStringToObject(root, "mac", GetMac().c_str());
    cJSON_AddStringToObject(root, "uuid", Board::GetInstance().GetUuid().c_str());
    cJSON_AddStringToObject(root, "ssid", GetSsid().c_str());
    cJSON_AddStringToObject(root, "device_name", DeviceNameLoad().c_str());
    LlmConfig lcfg = LlmConfigLoad();
    cJSON_AddStringToObject(root, "provider", lcfg.provider.c_str());
    cJSON_AddBoolToObject(root, "session_active", s_protocol && s_protocol->IsAudioChannelOpened());
    cJSON_AddBoolToObject(root, "camera_on", VisionModuleIsRunning());
    char* str = cJSON_PrintUnformatted(root);
    SendJson(req, str ? str : "{}");
    free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t HandleGetConfig(httpd_req_t* req) {
    cJSON* root = BuildConfigJson(false);
    char* str = cJSON_PrintUnformatted(root);
    SendJson(req, str ? str : "{}");
    free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t HandleExportConfig(httpd_req_t* req) {
    cJSON* root = BuildConfigJson(true);
    char* str = cJSON_PrintUnformatted(root);
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=xiaozhi-config.json");
    SendJson(req, str ? str : "{}");
    free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t HandleImportConfig(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, body, 16384)) {
        SendJson(req, "{\"ok\":false,\"error\":\"empty or oversized body\"}", HTTPD_400);
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        SendJson(req, "{\"ok\":false,\"error\":\"invalid json\"}", HTTPD_400);
        return ESP_OK;
    }
    std::string error;
    const bool ok = ApplyConfigJson(root, error);
    cJSON_Delete(root);
    if (!ok) {
        std::string msg = "{\"ok\":false,\"error\":\"" + error + "\"}";
        SendJson(req, msg.c_str(), HTTPD_400);
        return ESP_OK;
    }
    SendJson(req, "{\"ok\":true,\"message\":\"配置已导入\"}");
    return ESP_OK;
}

static esp_err_t HandlePostConfig(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, body)) {
        SendJson(req, "{\"ok\":false,\"error\":\"empty or oversized body\"}", HTTPD_400);
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        SendJson(req, "{\"ok\":false,\"error\":\"invalid json\"}", HTTPD_400);
        return ESP_OK;
    }
    std::string error;
    const bool ok = ApplyConfigJson(root, error);
    cJSON_Delete(root);
    if (!ok) {
        std::string msg = "{\"ok\":false,\"error\":\"" + error + "\"}";
        SendJson(req, msg.c_str(), HTTPD_400);
        return ESP_OK;
    }
    SendJson(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t HandleLogs(httpd_req_t* req) {
    char query[32] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[8] = {};
        if (httpd_query_key_value(query, "clear", value, sizeof(value)) == ESP_OK) {
            LogClear();
        }
    }
    std::string logs = LogRead();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, logs.c_str(), logs.size());
    return ESP_OK;
}

// 连通性测试：千问用 DashScope 模型列表接口验证 Key
static esp_err_t HandleTest(httpd_req_t* req) {
    LlmConfig lcfg = LlmConfigLoad();
    // 页面上的表单值可覆盖已保存配置，便于保存前先验证。
    std::string body;
    if (ReadBody(req, body, 4096)) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root) {
            cJSON* llm = cJSON_GetObjectItem(root, "llm");
            if (llm) {
                lcfg.provider = JsonGetString(llm, "provider", lcfg.provider);
                std::string key = JsonGetString(llm, "api_key", "");
                if (!key.empty()) lcfg.api_key = key;
            }
            cJSON_Delete(root);
        }
    }
    if (lcfg.api_key.empty()) {
        SendJson(req, "{\"ok\":false,\"error\":\"请先填写 API Key\"}");
        return ESP_OK;
    }
    if (lcfg.provider == "qwen") {
        esp_http_client_config_t cfg = {};
        cfg.url = "https://dashscope.aliyuncs.com/compatible-mode/v1/models";
        cfg.timeout_ms = 8000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.skip_cert_common_name_check = true;
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == nullptr) {
            SendJson(req, "{\"ok\":false,\"error\":\"HTTP client init failed\"}");
            return ESP_OK;
        }
        esp_http_client_set_header(client, "Authorization", ("Bearer " + lcfg.api_key).c_str());
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (err == ESP_OK && status == 200) {
            SendJson(req, "{\"ok\":true,\"message\":\"千问 API Key 有效\"}");
        } else {
            std::string msg = "{\"ok\":false,\"error\":\"千问连接失败 (HTTP " + std::to_string(status) + ")\"}";
            SendJson(req, msg.c_str());
        }
        return ESP_OK;
    }
    if (lcfg.provider == "doubao") {
        // 火山鉴权接口差异较大，用真实语音会话验证更可靠
        SendJson(req, "{\"ok\":true,\"message\":\"豆包请通过一次语音会话验证 Key\"}");
        return ESP_OK;
    }
    SendJson(req, "{\"ok\":false,\"error\":\"DeepSeek 语音 API 尚未开放\"}");
    return ESP_OK;
}

static esp_err_t HandleSession(httpd_req_t* req) {
    if (!s_protocol) {
        SendJson(req, "{\"ok\":false,\"error\":\"protocol unavailable\"}");
        return ESP_OK;
    }
    std::string body;
    if (!ReadBody(req, body, 1024)) {
        SendJson(req, "{\"ok\":false,\"error\":\"empty body\"}", HTTPD_400);
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    std::string action = root ? JsonGetString(root, "action", "") : "";
    if (root) cJSON_Delete(root);

    // 通过 Application 事件在主任务中切换会话，避免在 HTTP 任务中直接操作状态机。
    if (action == "start") {
        Application::GetInstance().StartListening();
        SendJson(req, "{\"ok\":true,\"message\":\"已请求开始会话\"}");
    } else if (action == "stop") {
        Application::GetInstance().StopConversation();
        SendJson(req, "{\"ok\":true,\"message\":\"已请求结束会话\"}");
    } else {
        SendJson(req, "{\"ok\":false,\"error\":\"action must be start/stop\"}", HTTPD_400);
    }
    return ESP_OK;
}

static esp_err_t HandleCamera(httpd_req_t* req) {
    if (!s_protocol) {
        SendJson(req, "{\"ok\":false,\"error\":\"unavailable\"}");
        return ESP_OK;
    }
    std::string body;
    if (!ReadBody(req, body, 1024)) {
        SendJson(req, "{\"ok\":false,\"error\":\"empty body\"}", HTTPD_400);
        return ESP_OK;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    std::string action = root ? JsonGetString(root, "action", "") : "";
    if (root) cJSON_Delete(root);

    if (action == "open") {
        VisionConfig vcfg = VisionConfigLoad();
        vcfg.enable = true;
        VisionConfigSave(vcfg);
        // 需求 FR-5.2：摄像头状态变化给出屏幕反馈（显示操作排到主任务执行）。
        bool ok = VisionModuleStart();
        NotifyDisplay(ok ? "摄像头已打开" : "摄像头打开失败，请检查接线");
        SendJson(req, ok ? "{\"ok\":true,\"message\":\"摄像头已打开\"}"
                         : "{\"ok\":false,\"error\":\"摄像头打开失败（检查接线）\"}");
    } else if (action == "close") {
        VisionConfig vcfg = VisionConfigLoad();
        vcfg.enable = false;
        VisionConfigSave(vcfg);
        VisionModuleStop();
        NotifyDisplay("摄像头已关闭");
        SendJson(req, "{\"ok\":true,\"message\":\"摄像头已关闭\"}");
    } else if (action == "snap") {
        Application::GetInstance().SnapAndAnalyze();
        SendJson(req, "{\"ok\":true,\"message\":\"拍照分析已触发\"}");
    } else {
        SendJson(req, "{\"ok\":false,\"error\":\"action must be open/close/snap\"}", HTTPD_400);
    }
    return ESP_OK;
}

static esp_err_t HandleReset(httpd_req_t* req) {
    {
        Settings llm("llm", true);
        llm.EraseAll();
    }
    {
        Settings vision("vision", true);
        vision.EraseAll();
    }
    {
        // 服务器模式遗留配置，直连模式不再使用。
        Settings wifi("wifi", true);
        wifi.EraseKey("ota_url");
    }
    {
        Settings device("device", true);
        device.EraseAll();
    }
    SendJson(req, "{\"ok\":true,\"message\":\"已恢复默认配置，设备即将重启\"}");
    Application::GetInstance().Schedule([]() { Application::GetInstance().Reboot(); });
    return ESP_OK;
}

static esp_err_t HandleReboot(httpd_req_t* req) {
    SendJson(req, "{\"ok\":true,\"message\":\"设备即将重启\"}");
    Application::GetInstance().Schedule([]() { Application::GetInstance().Reboot(); });
    return ESP_OK;
}

static esp_err_t HandleCameraPreview(httpd_req_t* req) {
    size_t len = 0;
    uint8_t* jpeg = VisionModulePreviewJpeg(&len);
    if (!jpeg || len == 0) {
        if (jpeg) free(jpeg);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no frame");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char*)jpeg, len);
    free(jpeg);
    return ESP_OK;
}

// ---------------- 页面 ----------------

static const char* PAGE_HTML = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>xiaozhiPro 直连配置</title>
<style>
:root{--bg:#0f1420;--card:#1a2233;--line:#2a3550;--txt:#e8ecf4;--sub:#8b96ad;--acc:#4f8cff;--ok:#2ecc71;--err:#e74c3c}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--txt);font-family:system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;padding:16px;min-height:100vh}
.wrap{max-width:720px;margin:0 auto}
header{display:flex;align-items:center;gap:12px;margin-bottom:20px;flex-wrap:wrap}
h1{font-size:22px;font-weight:700}
.badge{background:#223;color:var(--sub);border:1px solid var(--line);border-radius:999px;padding:2px 10px;font-size:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin-bottom:14px}
.card h2{font-size:15px;margin-bottom:12px;color:var(--acc)}
.row{display:flex;align-items:center;gap:10px;margin-bottom:10px;flex-wrap:wrap}
.row label{min-width:110px;font-size:13px;color:var(--sub)}
.row input,.row select{flex:1;min-width:160px;background:#10182a;border:1px solid var(--line);border-radius:8px;color:var(--txt);padding:9px 10px;font-size:14px;outline:none}
.row input:focus,.row select:focus{border-color:var(--acc)}
.row input[type=checkbox]{flex:0;min-width:auto;width:18px;height:18px;accent-color:var(--acc)}
.switch{display:flex;align-items:center;gap:8px}
.btns{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px}
button{background:var(--acc);border:none;border-radius:8px;color:#fff;padding:10px 16px;font-size:14px;cursor:pointer}
button:hover{filter:brightness(1.1)}
button.ghost{background:transparent;border:1px solid var(--line);color:var(--sub)}
button.danger{background:var(--err)}
button:disabled{opacity:.5;cursor:not-allowed}
#toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%);background:#0b1220;border:1px solid var(--line);border-radius:10px;padding:10px 18px;font-size:13px;display:none;z-index:99;max-width:90vw}
#toast.show{display:block}
.status{display:inline-flex;align-items:center;gap:6px;font-size:13px;color:var(--sub)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--sub)}
.dot.on{background:var(--ok)}.dot.off{background:var(--sub)}.dot.err{background:var(--err)}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
@media(max-width:560px){.grid2{grid-template-columns:1fr}}
img.preview{max-width:100%;border-radius:8px;border:1px solid var(--line);margin-top:10px;display:none}
#logs{background:#0b1220;border:1px solid var(--line);border-radius:8px;padding:10px;font:12px/1.5 ui-monospace,Consolas,monospace;color:#b9c4d8;max-height:260px;overflow:auto;white-space:pre-wrap;word-break:break-all;margin-top:10px}
.muted{color:var(--sub);font-size:12px}
</style>
</head>
<body>
<div class="wrap">
<header>
<h1>xiaozhiPro 直连配置</h1>
<span class="badge" id="ip">IP: --</span>
<span class="badge" id="fw">--</span>
</header>

<div class="card">
<h2>设备状态</h2>
<div class="row"><label>WiFi</label><span class="status" id="wifi">--</span></div>
<div class="row"><label>设备名称</label><span class="status" id="devname">--</span></div>
<div class="row"><label>MAC</label><span class="status" id="mac">--</span></div>
<div class="row"><label>UUID</label><span class="status" id="uuid">--</span></div>
<div class="row"><label>会话</label><span class="status"><span class="dot" id="sessDot"></span><span id="sess">未连接</span></span></div>
<div class="row"><label>摄像头</label><span class="status"><span class="dot" id="camDot"></span><span id="cam">关闭</span></span></div>
<div class="btns">
<button onclick="session('start')">开始会话</button>
<button class="ghost" onclick="session('stop')">结束会话</button>
<button onclick="cam('open')">打开摄像头</button>
<button class="ghost" onclick="cam('close')">关闭摄像头</button>
<button class="ghost" onclick="cam('snap')">拍照识别</button>
</div>
</div>

<div class="card">
<h2>设备参数</h2>
<div class="row"><label>设备名称</label><input type="text" id="dev_name" maxlength="32" placeholder="给小智起个名字"></div>
<div class="row"><label>输出音量</label><input type="number" id="dev_volume" min="0" max="100"></div>
<div class="btns">
<button class="ghost" onclick="saveDevice()">保存设备参数</button>
</div>
</div>

<div class="card">
<h2>AI 服务商（纯直连，无服务器）</h2>
<div class="row"><label>服务商</label>
<select id="provider" onchange="providerChanged()">
<option value="qwen">阿里千问（Qwen-Omni Realtime）</option>
<option value="doubao">字节豆包（端到端实时语音）</option>
<option value="deepseek" disabled>DeepSeek（预留，待语音 API）</option>
</select></div>
<div class="row"><label>API Key</label><input type="password" id="api_key" placeholder="留空表示不修改"></div>
<div class="row switch"><label>清除已保存的 Key</label><input type="checkbox" id="clear_key"></div>
<div class="row"><label>模型</label><input type="text" id="model" list="model_list" placeholder="qwen-omni-turbo-realtime"></div>
<datalist id="model_list">
<option value="qwen-omni-turbo-realtime">
<option value="qwen2.5-omni-7b">
<option value="doubao-realtime-3.0">
</datalist>
<div class="row"><label>接入端点</label><input type="text" id="base_url"></div>
<div class="grid2">
<div class="row"><label>输出格式</label><select id="audio_format"><option>pcm</option><option>wav</option><option>ogg_opus</option></select></div>
<div class="row"><label>音色</label><input type="text" id="voice" placeholder="Chelsie"></div>
<div class="row"><label>语言</label><input type="text" id="language" placeholder="zh"></div>
<div class="row"><label>空闲超时(s)</label><input type="number" id="session_timeout" min="5" max="600"></div>
</div>
<div class="row switch"><label>允许打断</label><input type="checkbox" id="interrupt_enable"></div>
<div class="btns">
<button onclick="save()">保存 AI 配置</button>
<button class="ghost" onclick="testConn()">测试连接</button>
</div>
<span class="muted">API Key 明文保存在设备 NVS 中，页面不回显；局域网 HTTP 无加密。</span>
<br><span class="muted">唤醒词在编译期确定，网页不可修改；视觉能力当前仅千问支持。</span>
</div>

<div class="card">
<h2>摄像头 / 视频识别（OV2640）</h2>
<div class="row switch"><label>开机自启</label><input type="checkbox" id="v_enable"></div>
<div class="grid2">
<div class="row"><label>宽度</label><input type="number" id="v_width" min="160" max="1600" step="8"></div>
<div class="row"><label>高度</label><input type="number" id="v_height" min="120" max="1200" step="8"></div>
<div class="row"><label>JPEG 质量</label><input type="number" id="v_quality" min="10" max="100"></div>
<div class="row"><label>触发模式</label><select id="v_trigger"><option>voice</option><option>manual</option><option>both</option></select></div>
<div class="row"><label>开关指令词</label><input type="text" id="v_switch"></div>
<div class="row"><label>识别指令词</label><input type="text" id="v_command"></div>
<div class="row"><label>帧间隔(ms)</label><input type="number" id="v_interval" min="500" max="10000" step="100"></div>
<div class="row"><label>自动关闭(ms)</label><input type="number" id="v_auto_off" min="0" step="1000"></div>
</div>
<div class="row switch"><label>连续帧分析</label><input type="checkbox" id="v_continuous"></div>
<div class="row"><label>播报模式</label>
<select id="v_announce">
<option value="quiet">quiet（静默，提问时回答）</option>
<option value="periodic">periodic（周期播报）</option>
<option value="event">event（变化/异常播报）</option>
</select></div>
<div class="btns">
<button onclick="saveVision()">保存配置</button>
<button class="ghost" onclick="refreshPreview()">刷新预览</button>
</div>
<img class="preview" id="preview" alt="camera preview">
</div>

<div class="card">
<h2>运行日志</h2>
<span class="muted">串口日志镜像，仅保留最近 8KB，用于排障。</span>
<pre id="logs">点击「刷新日志」查看</pre>
<div class="btns">
<button class="ghost" onclick="loadLogs()">刷新日志</button>
<button class="ghost" onclick="clearLogs()">清空日志</button>
</div>
</div>

<div class="card">
<h2>运维</h2>
<div class="btns">
<button onclick="saveAllAndReboot()">保存全部并重启</button>
<button class="ghost" onclick="saveAll()">仅保存全部</button>
<button class="ghost" onclick="exportConfig()">导出配置</button>
<button class="ghost" onclick="document.getElementById('import_file').click()">导入配置</button>
<input type="file" id="import_file" accept="application/json,.json" style="display:none" onchange="importConfig(this)">
<button class="ghost" onclick="resetConfig()">恢复默认配置并重启</button>
<button class="danger" onclick="reboot()">重启设备</button>
</div>
<span class="muted">导出文件中的 API Key 已脱敏；导入时 Key 为 ******** 或留空表示保留设备现有 Key。</span>
</div>
</div>

<div id="toast"></div>
<script>
function toast(msg, err){var t=document.getElementById('toast');t.textContent=msg;t.style.borderColor=err?'var(--err)':'var(--line)';t.classList.add('show');setTimeout(function(){t.classList.remove('show')},3000)}
async function api(path, body){var r=await fetch(path,{method:body?'POST':'GET',body:body?JSON.stringify(body):undefined,headers:body?{'Content-Type':'application/json'}:{}});return r.json()}
async function refresh(){
  try{
    var s=await api('/api/status');
    document.getElementById('ip').textContent='IP: '+s.ip;
    document.getElementById('fw').textContent=s.fw_version;
    document.getElementById('wifi').textContent=s.ssid||'--';
    document.getElementById('mac').textContent=s.mac||'--';
    document.getElementById('uuid').textContent=s.uuid||'--';
    document.getElementById('devname').textContent=s.device_name||'(未命名)';
    var sd=document.getElementById('sessDot'),sc=document.getElementById('sess');
    sd.className='dot '+(s.session_active?'on':'off');sc.textContent=s.session_active?'已连接('+s.provider+')':'未连接';
    var cd=document.getElementById('camDot'),cc=document.getElementById('cam');
    cd.className='dot '+(s.camera_on?'on':'off');cc.textContent=s.camera_on?'打开':'关闭';
  }catch(e){}
}
async function loadConfig(){
  try{
    var c=await api('/api/config');
    document.getElementById('provider').value=c.llm.provider;
    document.getElementById('model').value=c.llm.model;
    document.getElementById('base_url').value=c.llm.base_url;
    document.getElementById('audio_format').value=c.llm.audio_format;
    document.getElementById('voice').value=c.llm.voice;
    document.getElementById('language').value=c.llm.language;
    document.getElementById('session_timeout').value=c.llm.session_timeout;
    document.getElementById('interrupt_enable').checked=c.llm.interrupt_enable;
    document.getElementById('api_key').placeholder=c.llm.has_api_key?'已设置，留空不修改':'未设置';
    document.getElementById('v_enable').checked=c.vision.enable;
    document.getElementById('v_width').value=c.vision.width;
    document.getElementById('v_height').value=c.vision.height;
    document.getElementById('v_quality').value=c.vision.quality;
    document.getElementById('v_trigger').value=c.vision.trigger_mode;
    document.getElementById('v_switch').value=c.vision.switch_phrases;
    document.getElementById('v_command').value=c.vision.command_phrases;
    document.getElementById('v_interval').value=c.vision.frame_interval_ms;
    document.getElementById('v_auto_off').value=c.vision.auto_off_ms;
    document.getElementById('v_continuous').checked=c.vision.continuous_enable;
    document.getElementById('v_announce').value=c.vision.announce_mode;
    document.getElementById('dev_name').value=(c.device&&c.device.name)||'';
    document.getElementById('dev_volume').value=(c.device&&c.device.volume!=null)?c.device.volume:70;
  }catch(e){toast('加载配置失败',true)}
}
// 服务商默认值（FR-3.2）：切换服务商时带出官方端点与推荐模型。
var PROVIDER_DEFAULTS={
  qwen:{model:'qwen-omni-turbo-realtime',url:'wss://dashscope.aliyuncs.com/api-ws/v1/realtime',format:'pcm',voice:'Chelsie',lang:'zh'},
  doubao:{model:'doubao-realtime-3.0',url:'wss://openspeech.bytedance.com/api/v3/realtime_voice',format:'pcm',voice:'',lang:'zh'}
};
function providerChanged(){
  var d=PROVIDER_DEFAULTS[document.getElementById('provider').value];
  if(!d)return;
  document.getElementById('model').value=d.model;
  document.getElementById('base_url').value=d.url;
  document.getElementById('audio_format').value=d.format;
  document.getElementById('voice').value=d.voice;
  document.getElementById('language').value=d.lang;
}
function llmBody(){
  return {
    provider:document.getElementById('provider').value,
    api_key:document.getElementById('api_key').value,
    clear_api_key:document.getElementById('clear_key').checked,
    model:document.getElementById('model').value,
    base_url:document.getElementById('base_url').value,
    audio_format:document.getElementById('audio_format').value,
    voice:document.getElementById('voice').value,
    language:document.getElementById('language').value,
    session_timeout:parseInt(document.getElementById('session_timeout').value)||60,
    interrupt_enable:document.getElementById('interrupt_enable').checked
  };
}
function deviceBody(){
  return {
    name:document.getElementById('dev_name').value,
    volume:parseInt(document.getElementById('dev_volume').value)
  };
}
async function save(){
  var r=await api('/api/config',{llm:llmBody()});
  toast(r.ok?'AI 配置已保存':'保存失败：'+r.error,r.ok?false:true);
  refresh();
}
async function saveDevice(){
  var r=await api('/api/config',{device:deviceBody()});
  toast(r.ok?'设备参数已保存':'保存失败：'+r.error,r.ok?false:true);
  refresh();
}
async function saveAll(){
  var r=await api('/api/config',{llm:llmBody(),vision:visionBody(),device:deviceBody()});
  if(r.ok)refresh();
  return r;
}
async function saveAllAndReboot(){
  if(!confirm('保存全部配置并重启设备？'))return;
  var r=await saveAll();
  if(!r.ok){toast('保存失败：'+r.error,true);return}
  toast('配置已保存，设备正在重启');
  await api('/api/reboot',{});
}
function visionBody(){
  return {
    enable:document.getElementById('v_enable').checked,
    width:parseInt(document.getElementById('v_width').value)||640,
    height:parseInt(document.getElementById('v_height').value)||480,
    quality:parseInt(document.getElementById('v_quality').value)||60,
    trigger_mode:document.getElementById('v_trigger').value,
    switch_phrases:document.getElementById('v_switch').value,
    command_phrases:document.getElementById('v_command').value,
    continuous_enable:document.getElementById('v_continuous').checked,
    frame_interval_ms:parseInt(document.getElementById('v_interval').value)||2000,
    announce_mode:document.getElementById('v_announce').value,
    auto_off_ms:parseInt(document.getElementById('v_auto_off').value)||0
  };
}
async function saveVision(){
  var r=await api('/api/config',{vision:visionBody()});
  toast(r.ok?'摄像头配置已保存':'保存失败：'+r.error,r.ok?false:true);
  refresh();
}
async function testConn(){var body={llm:{provider:document.getElementById('provider').value,api_key:document.getElementById('api_key').value,model:document.getElementById('model').value,base_url:document.getElementById('base_url').value}};var r=await api('/api/test',body);toast(r.ok?r.message:'失败：'+r.error,!r.ok)}
async function session(a){var r=await api('/api/session',{action:a});toast(r.message||r.error,!r.ok);refresh()}
async function cam(a){var r=await api('/api/camera',{action:a});toast(r.message||r.error,!r.ok);refresh()}
function refreshPreview(){var img=document.getElementById('preview');img.src='/api/preview?t='+Date.now();img.style.display='block'}
async function resetConfig(){if(!confirm('确定恢复默认配置？设备将重启。'))return;var r=await api('/api/reset',{});toast(r.message||r.error,!r.ok)}
async function reboot(){if(!confirm('确定重启设备？'))return;var r=await api('/api/reboot',{});toast(r.message||r.error,!r.ok)}
function exportConfig(){fetch('/api/export').then(function(r){return r.blob()}).then(function(b){var a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='xiaozhi-config.json';document.body.appendChild(a);a.click();document.body.removeChild(a);setTimeout(function(){URL.revokeObjectURL(a.href)},1000)})}
async function importConfig(input){
  var file=input.files&&input.files[0];
  if(!file)return;
  try{
    var obj=JSON.parse(await file.text());
    var r=await api('/api/import',obj);
    toast(r.ok?'配置已导入':'导入失败：'+r.error,!r.ok);
    if(r.ok){await loadConfig();refresh()}
  }catch(e){toast('导入失败：文件不是有效的 JSON',true)}
  input.value='';
}
async function loadLogs(){try{var r=await fetch('/api/logs');var t=await r.text();var box=document.getElementById('logs');box.textContent=t||'(暂无日志)';box.scrollTop=box.scrollHeight}catch(e){toast('读取日志失败',true)}}
async function clearLogs(){try{await fetch('/api/logs?clear=1');document.getElementById('logs').textContent='(暂无日志)'}catch(e){}}
refresh();loadConfig();loadLogs();setInterval(refresh,10000);
</script>
</body>
</html>
)HTML";

static esp_err_t HandlePage(httpd_req_t* req) {
    // 需求 FR-1.5(a)：配置网页被访问后退出屏幕 IP 引导，并记住设置已完成。
    IpHint::MarkConfigured();
    IpHint::Hide("config page accessed");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, PAGE_HTML);
    return ESP_OK;
}

static void RegisterHandler(httpd_handle_t server, const char* uri,
                            httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t h = {};
    h.uri = uri;
    h.method = method;
    h.handler = handler;
    h.user_ctx = nullptr;
    httpd_register_uri_handler(server, &h);
}

bool ConfigServerInit(DirectVoiceProtocol* protocol) {
    // 协议对象可能因重连而重建，始终刷新指针。
    s_protocol = protocol;
    if (s_server) return true;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    // /api/test 会在此任务内执行 HTTPS 请求（TLS 握手栈开销较大）。
    cfg.stack_size = 10240;
    cfg.max_uri_handlers = 20;

    // 串口日志镜像（FR-6.4）必须在 httpd 启动前安装，才能捕获启动日志。
    LogHookInstall();

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        LogHookRestore();
        return false;
    }

    RegisterHandler(s_server, "/", HTTP_GET, HandlePage);
    RegisterHandler(s_server, "/api/status", HTTP_GET, HandleStatus);
    RegisterHandler(s_server, "/api/config", HTTP_GET, HandleGetConfig);
    RegisterHandler(s_server, "/api/config", HTTP_POST, HandlePostConfig);
    RegisterHandler(s_server, "/api/test", HTTP_POST, HandleTest);
    RegisterHandler(s_server, "/api/session", HTTP_POST, HandleSession);
    RegisterHandler(s_server, "/api/camera", HTTP_POST, HandleCamera);
    // 需求文档 §3.5 约定 GET /api/camera 返回预览；保留 /api/preview 作为页面实际使用路径。
    RegisterHandler(s_server, "/api/camera", HTTP_GET, HandleCameraPreview);
    RegisterHandler(s_server, "/api/preview", HTTP_GET, HandleCameraPreview);
    RegisterHandler(s_server, "/api/reset", HTTP_POST, HandleReset);
    RegisterHandler(s_server, "/api/reboot", HTTP_POST, HandleReboot);
    // 配置导出/导入（FR-6.3）与串口日志镜像（FR-6.4）
    RegisterHandler(s_server, "/api/export", HTTP_GET, HandleExportConfig);
    RegisterHandler(s_server, "/api/import", HTTP_POST, HandleImportConfig);
    RegisterHandler(s_server, "/api/logs", HTTP_GET, HandleLogs);

    ESP_LOGI(TAG, "config server started on port 80");
    return true;
}

void ConfigServerDeinit() {
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
    LogHookRestore();
    s_protocol = nullptr;
}
