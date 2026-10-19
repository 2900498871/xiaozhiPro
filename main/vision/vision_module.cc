#include "vision_module.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include "ai_config.h"

#define TAG "VisionModule"

#if CONFIG_IDF_TARGET_ESP32S3

#include <driver/ledc.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>

static std::atomic<bool> s_running{false};
static std::atomic<bool> s_stop_requested{false};
static SemaphoreHandle_t s_mutex = nullptr;
static bool s_camera_inited = false;
static uint8_t* s_preview_jpeg = nullptr;  // 最近一帧缓存（PSRAM 分配）
static size_t s_preview_len = 0;
static VisionFrameCallback s_frame_cb = nullptr;
static std::atomic<TaskHandle_t> s_analyze_task{nullptr};
static int64_t s_start_us = 0;

static uint8_t* AllocJpeg(size_t len) {
    uint8_t* p = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = (uint8_t*)malloc(len);
    }
    return p;
}

static framesize_t MapFrameSize(int width, int height) {
    if (width <= 160 && height <= 120) return FRAMESIZE_QQVGA;
    if (width <= 320 && height <= 240) return FRAMESIZE_QVGA;
    if (width <= 640 && height <= 480) return FRAMESIZE_VGA;
    if (width <= 800 && height <= 600) return FRAMESIZE_SVGA;
    if (width <= 1024 && height <= 768) return FRAMESIZE_XGA;
    if (width <= 1280 && height <= 1024) return FRAMESIZE_SXGA;
    return FRAMESIZE_UXGA;
}

// 幂等清理：调用方需保证分析任务已退出或就是当前任务。
static void VisionCleanupLocked() {
    if (s_camera_inited) {
        esp_camera_deinit();
        s_camera_inited = false;
    }
    if (s_preview_jpeg) {
        free(s_preview_jpeg);
        s_preview_jpeg = nullptr;
        s_preview_len = 0;
    }
}

// 安全拷贝回调：摄像头任务与协议层可能并发设置/读取。s_mutex 为 nullptr 时
// 摄像头任务尚未创建，不存在并发访问。
static VisionFrameCallback GetFrameCallback() {
    if (!s_mutex) return s_frame_cb;
    VisionFrameCallback cb;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cb = s_frame_cb;
        xSemaphoreGive(s_mutex);
    }
    return cb;
}

static void UpdatePreview(const camera_fb_t* fb) {
    uint8_t* copy = AllocJpeg(fb->len);
    if (!copy) return;
    memcpy(copy, fb->buf, fb->len);
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        free(s_preview_jpeg);
        s_preview_jpeg = copy;
        s_preview_len = fb->len;
        xSemaphoreGive(s_mutex);
    } else {
        free(copy);
    }
}

static void CameraTask(void* arg) {
    (void)arg;
    s_start_us = esp_timer_get_time();

    while (!s_stop_requested.load()) {
        // 每轮重新读取配置，使网页修改帧间隔/连续分析后无需重启摄像头即可生效。
        VisionConfig vcfg = VisionConfigLoad();
        int interval_ms = vcfg.frame_interval_ms > 0 ? vcfg.frame_interval_ms : 2000;
        int auto_off_ms = vcfg.auto_off_ms;

        // 100ms 粒度等待，保证停止指令响应及时。
        int waited = 0;
        while (waited < interval_ms && !s_stop_requested.load()) {
            int slice = interval_ms - waited;
            if (slice > 100) slice = 100;
            vTaskDelay(pdMS_TO_TICKS(slice));
            waited += slice;
        }
        if (s_stop_requested.load()) break;

        if (auto_off_ms > 0 &&
            (esp_timer_get_time() - s_start_us) >= (int64_t)auto_off_ms * 1000) {
            ESP_LOGI(TAG, "auto off after %d ms", auto_off_ms);
            break;
        }

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "fb get failed");
            continue;
        }
        UpdatePreview(fb);

        VisionFrameCallback cb = GetFrameCallback();
        if (cb && vcfg.continuous_enable && fb->buf && fb->len > 0) {
            cb(fb->buf, fb->len);
        }
        esp_camera_fb_return(fb);
    }

    // 任务自行退出：关闭摄像头并释放缓存（voice stop 会等待本标志）。
    s_analyze_task.store(nullptr);
    s_running.store(false);
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        VisionCleanupLocked();
        xSemaphoreGive(s_mutex);
    }
    s_stop_requested.store(false);
    ESP_LOGI(TAG, "vision task exited");
    vTaskDelete(nullptr);
}

static bool CameraInit() {
    camera_config_t cfg = {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.pin_d0 = CAM_PIN_D0;
    cfg.pin_d1 = CAM_PIN_D1;
    cfg.pin_d2 = CAM_PIN_D2;
    cfg.pin_d3 = CAM_PIN_D3;
    cfg.pin_d4 = CAM_PIN_D4;
    cfg.pin_d5 = CAM_PIN_D5;
    cfg.pin_d6 = CAM_PIN_D6;
    cfg.pin_d7 = CAM_PIN_D7;
    cfg.pin_xclk = CAM_PIN_XCLK;
    cfg.pin_pclk = CAM_PIN_PCLK;
    cfg.pin_vsync = CAM_PIN_VSYNC;
    cfg.pin_href = CAM_PIN_HREF;
    cfg.pin_sccb_sda = CAM_PIN_SIOD;
    cfg.pin_sccb_scl = CAM_PIN_SIOC;
    cfg.pin_pwdn = CAM_PIN_PWDN;
    cfg.pin_reset = CAM_PIN_RESET;
    cfg.xclk_freq_hz = CAM_XCLK_FREQ;
    cfg.pixel_format = PIXFORMAT_JPEG;

    const bool has_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    cfg.fb_location = has_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    cfg.fb_count = has_psram ? 2 : 1;
    cfg.grab_mode = has_psram ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;

    VisionConfig vcfg = VisionConfigLoad();
    cfg.frame_size = MapFrameSize(vcfg.width, vcfg.height);
    // 网页配置为"质量百分比"（越大越好），esp32-camera 使用 0~63 且越小越好。
    int quality = vcfg.quality;
    if (quality < 10) quality = 10;
    if (quality > 100) quality = 100;
    cfg.jpeg_quality = 63 - (quality * 63) / 100;
    if (cfg.jpeg_quality < 0) cfg.jpeg_quality = 0;
    if (cfg.jpeg_quality > 63) cfg.jpeg_quality = 63;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return false;
    }
    s_camera_inited = true;
    return true;
}

bool VisionModuleStart() {
    if (s_running.load()) return true;
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return false;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    if (s_running.load()) {
        xSemaphoreGive(s_mutex);
        return true;
    }
    s_stop_requested.store(false);
    bool ok = CameraInit();
    xSemaphoreGive(s_mutex);
    if (!ok) return false;

    s_running.store(true);
    TaskHandle_t handle = nullptr;
    BaseType_t created = xTaskCreate(CameraTask, "vision_analyze", 6144, nullptr, 5, &handle);
    s_analyze_task.store(handle);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "vision task create failed");
        s_analyze_task.store(nullptr);
        s_running.store(false);
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            VisionCleanupLocked();
            xSemaphoreGive(s_mutex);
        }
        return false;
    }
    VisionConfig vcfg = VisionConfigLoad();
    ESP_LOGI(TAG, "vision started (%dx%d, q=%d)", vcfg.width, vcfg.height, vcfg.quality);
    return true;
}

void VisionModuleStop() {
    if (!s_running.load() && s_analyze_task.load() == nullptr) return;

    s_stop_requested.store(true);

    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    TaskHandle_t task = s_analyze_task.load();
    if (task != nullptr && task != current) {
        // 等待分析任务自行退出（最多 2 秒）。
        for (int i = 0; i < 20 && s_analyze_task.load() != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        task = s_analyze_task.load();
        if (task != nullptr) {
            ESP_LOGW(TAG, "force deleting vision task");
            vTaskDelete(task);
            s_analyze_task.store(nullptr);
            s_running.store(false);
        }
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        VisionCleanupLocked();
        xSemaphoreGive(s_mutex);
    }
    s_stop_requested.store(false);
    ESP_LOGI(TAG, "vision stopped (camera powered off)");
}

uint8_t* VisionModuleSnapshotJpeg(size_t* len) {
    if (len) *len = 0;
    if (!s_running.load()) return nullptr;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return nullptr;
    if (!s_camera_inited) {
        xSemaphoreGive(s_mutex);
        return nullptr;
    }

    uint8_t* out = nullptr;
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
        out = AllocJpeg(fb->len);
        if (out) {
            memcpy(out, fb->buf, fb->len);
            if (len) *len = fb->len;
        }
        esp_camera_fb_return(fb);
    }
    xSemaphoreGive(s_mutex);
    return out;
}

bool VisionModuleIsRunning() { return s_running.load(); }

void VisionModuleSetFrameCallback(VisionFrameCallback cb) {
    if (!s_mutex) {
        s_frame_cb = std::move(cb);
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        s_frame_cb = std::move(cb);
        xSemaphoreGive(s_mutex);
    } else {
        ESP_LOGE(TAG, "failed to update frame callback (mutex busy)");
    }
}

uint8_t* VisionModulePreviewJpeg(size_t* len) {
    if (len) *len = 0;
    if (!s_mutex || !s_running.load()) return nullptr;

    uint8_t* out = nullptr;
    size_t out_len = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_preview_jpeg && s_preview_len > 0) {
            out = AllocJpeg(s_preview_len);
            if (out) {
                memcpy(out, s_preview_jpeg, s_preview_len);
                out_len = s_preview_len;
            }
        }
        xSemaphoreGive(s_mutex);
    }
    if (!out) {
        // 缓存尚未生成（刚启动或连续模式关闭）：即时抓取一帧。
        out = VisionModuleSnapshotJpeg(&out_len);
        if (!out) return nullptr;
    }
    if (len) *len = out_len;
    return out;
}

#else  // !CONFIG_IDF_TARGET_ESP32S3

bool VisionModuleStart() {
    ESP_LOGW(TAG, "camera is only supported on ESP32-S3 targets");
    return false;
}
void VisionModuleStop() {}
uint8_t* VisionModuleSnapshotJpeg(size_t* len) {
    if (len) *len = 0;
    return nullptr;
}
bool VisionModuleIsRunning() { return false; }
void VisionModuleSetFrameCallback(VisionFrameCallback cb) { (void)cb; }
uint8_t* VisionModulePreviewJpeg(size_t* len) {
    if (len) *len = 0;
    return nullptr;
}

#endif
