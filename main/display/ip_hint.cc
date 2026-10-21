#include "ip_hint.h"

#include <cstdio>
#include <mutex>
#include <string>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>

#include "application.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#define TAG "IpHint"

namespace {

// 引导最长占用屏幕的时间（FR-1.5 退出条件 d）。
constexpr int64_t kTimeoutUs = 10LL * 60 * 1000000;

std::mutex s_mutex;
bool s_active = false;
int64_t s_start_us = 0;
std::string s_url;   // 已解析的配置页地址
std::string s_text;  // 屏幕上应显示的完整文案
std::string s_shown; // 最近一次写入屏幕的文案（用于精确移除）

std::string NetifIp(const char* ifkey) {
    esp_netif_ip_info_t info = {};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return "";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    return buf;
}

// 优先设备自身的 STA 地址；配网热点模式下退回到热点网关地址。
std::string ResolveUrl() {
    std::string sta = NetifIp("WIFI_STA_DEF");
    if (!sta.empty()) {
        return "http://" + sta;
    }
    std::string ap = NetifIp("WIFI_AP_DEF");
    if (!ap.empty()) {
        return "http://" + ap;
    }
    return "";
}

bool DeviceIdle() {
    return Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
}

bool AlreadyConfigured() {
    Settings settings("device", false);
    return settings.GetBool("configured", false);
}

void WriteToDisplay(const std::string& text) {
    Display* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    // OLED 的聊天标签是 LV_LABEL_LONG_SCROLL_CIRCULAR，长文案自动滚动。
    display->SetChatMessage("system", text.c_str());
}

void RemoveFromDisplay(const std::string& text) {
    if (text.empty()) {
        return;
    }
    Display* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    display->RemoveChatMessage("system", text.c_str());
}

// 必须在持有 s_mutex 时调用。
void UpdateTextLocked() {
    s_url = ResolveUrl();
    if (s_url.empty()) {
        s_text = "等待网络 ...";
    } else {
        s_text = "设置页 " + s_url;
    }
}

// 解析当前地址并按需重绘。只在 IDLE 状态占屏，避免覆盖开机/告警信息。
// 仅在文案变化（DHCP 换 IP）或主动要求时写屏：SetChatMessage 会重置跑马灯动画，
// 每秒重写会让滚动一直停在开头。
void Refresh(bool force) {
    std::string text;
    bool should_draw = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_active || !DeviceIdle()) {
            return;
        }
        UpdateTextLocked();
        if (force || s_text != s_shown) {
            text = s_text;
            s_shown = s_text;
            should_draw = true;
        }
    }
    if (should_draw) {
        WriteToDisplay(text);
    }
}

}  // namespace

namespace IpHint {

void Show() {
    // 配置页访问过一次即视为完成设置，后续开机的空闲态不再被引导占屏。
    if (AlreadyConfigured()) {
        ESP_LOGI(TAG, "skip config url hint: setup already completed");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_active = true;
        s_start_us = esp_timer_get_time();
        s_shown.clear();
    }
    ESP_LOGI(TAG, "show config url hint");
    Refresh(true);
}

void Hide(const char* reason) {
    std::string shown;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_active) {
            return;
        }
        s_active = false;
        shown.swap(s_shown);
        s_text.clear();
        s_url.clear();
    }
    ESP_LOGI(TAG, "hide config url hint (%s)", reason != nullptr ? reason : "");
    RemoveFromDisplay(shown);
}

bool IsActive() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_active;
}

void Tick() {
    bool should_hide = false;
    std::string shown;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_active) {
            return;
        }
        const int64_t now = esp_timer_get_time();
        const DeviceState state = Application::GetInstance().GetDeviceState();
        // 唤醒、开始对话或重新进入配网时让出屏幕；开机激活阶段不退出。
        const bool conversation_started =
            (state == kDeviceStateConnecting || state == kDeviceStateListening ||
             state == kDeviceStateSpeaking || state == kDeviceStateNotifying ||
             state == kDeviceStateWifiConfiguring);
        if (now - s_start_us >= kTimeoutUs || conversation_started) {
            s_active = false;
            shown.swap(s_shown);
            s_text.clear();
            s_url.clear();
            should_hide = true;
        }
    }
    if (should_hide) {
        ESP_LOGI(TAG, "config url hint timed out or conversation started");
        RemoveFromDisplay(shown);
        return;
    }
    Refresh(false);
}

void Redraw() {
    Refresh(true);
}

std::string Url() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_url;
}

void MarkConfigured() {
    // 页面会周期轮询 /api/status，已置位时不再写 NVS，避免磨损。
    if (AlreadyConfigured()) {
        return;
    }
    Settings settings("device", true);
    settings.SetBool("configured", true);
}

}  // namespace IpHint
