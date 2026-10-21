#include "direct_voice_protocol.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "adapters/adapter_interface.h"
#include "adapters/qwen_realtime.h"
#include "adapters/doubao_realtime.h"
#include "adapters/deepseek_adapter.h"
#include "application.h"
#include "board.h"
#include "display.h"
#include "vision/vision_module.h"

#define TAG "DirectVoiceProtocol"

static std::unique_ptr<VoiceAdapter> CreateVoiceAdapter(const std::string& provider) {
    if (provider == "qwen") {
        return std::make_unique<QwenRealtimeAdapter>();
    } else if (provider == "doubao") {
        return std::make_unique<DoubaoRealtimeAdapter>();
    } else if (provider == "deepseek") {
        return std::make_unique<DeepSeekAdapter>();
    }
    // 未知/空 provider：返回空，由上层报"未知服务商"，避免静默落到 DeepSeek 造成误导
    return nullptr;
}

// 将 "打开摄像头,关闭摄像头" 拆成列表
static std::vector<std::string> SplitPhrases(const std::string& phrases) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= phrases.size()) {
        size_t comma = phrases.find(',', start);
        std::string item = phrases.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!item.empty()) out.push_back(std::move(item));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

DirectVoiceProtocol::DirectVoiceProtocol() = default;
DirectVoiceProtocol::~DirectVoiceProtocol() {
    VisionModuleSetFrameCallback(nullptr);
    CloseAudioChannel();
}

bool DirectVoiceProtocol::Start() {
    cfg_ = LlmConfigLoad();
    provider_ = cfg_.provider;
    started_ = true;
    // 连续帧分析：每 frame_interval_ms 由摄像头任务回调一次
    VisionModuleSetFrameCallback(
        [this](const uint8_t* jpeg, size_t len) { OnVisionFrame(jpeg, len); });
    ESP_LOGI(TAG, "DirectVoiceProtocol started, provider=%s model=%s", provider_.c_str(),
             cfg_.model.c_str());
    return true;
}

bool DirectVoiceProtocol::OpenAudioChannel() {
    if (!started_) Start();
    // 网页保存配置后无需重启即可生效：每次建立会话前重新读取。
    cfg_ = LlmConfigLoad();
    provider_ = cfg_.provider;
    CreateAdapter();
    if (!adapter_) {
        SetError("未知的服务商配置（支持 qwen/doubao/deepseek），请检查网页设置");
        return false;
    }

    adapter_->on_audio = [this](const uint8_t* pcm, size_t len, int sample_rate) {
        OnAdapterAudio(pcm, len, sample_rate);
    };
    adapter_->on_text = [this](const std::string& text) { OnAdapterText(text); };
    adapter_->on_user_text = [this](const std::string& text) { OnUserText(text); };
    adapter_->on_state = [this](const std::string& state) { OnAdapterState(state); };
    adapter_->on_error = [this](const std::string& error) {
        ESP_LOGW(TAG, "adapter error: %s", error.c_str());
        SetError(error);
    };

    VoiceAdapterConfig acfg;
    acfg.provider = cfg_.provider;
    acfg.api_key = cfg_.api_key;
    acfg.model = cfg_.model;
    acfg.base_url = cfg_.base_url;
    acfg.audio_format = cfg_.audio_format;
    acfg.voice = cfg_.voice;
    acfg.language = cfg_.language;
    acfg.interrupt_enable = cfg_.interrupt_enable;
    acfg.session_timeout_s = cfg_.session_timeout_s;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        response_active_ = false;
        tts_started_ = false;
        response_text_.clear();
    }
    request_in_flight_.store(false);
    listening_.store(false);
    ResetVisionAnalysisState();

    if (!adapter_->Connect(acfg)) {
        SetError("连接语音服务失败");
        return false;
    }
    last_activity_us_.store(esp_timer_get_time());
    return true;
}

void DirectVoiceProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    if (adapter_) {
        adapter_->Close();
        adapter_.reset();
    }
    bool was_tts = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        was_tts = tts_started_;
        tts_started_ = false;
        response_active_ = false;
        response_text_.clear();
    }
    request_in_flight_.store(false);
    listening_.store(false);
    ResetVisionAnalysisState();
    if (was_tts) EmitTts("stop");
}

bool DirectVoiceProtocol::IsAudioChannelOpened() const {
    return adapter_ && adapter_->IsConnected();
}

bool DirectVoiceProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!adapter_ || !adapter_->IsConnected() || !packet) return false;
    last_activity_us_.store(esp_timer_get_time());
    return adapter_->SendAudio(packet->payload.data(), packet->payload.size(), packet->opus);
}

void DirectVoiceProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    listening_.store(true);
    last_activity_us_.store(esp_timer_get_time());
    // 新一轮输入开始：允许打断当前回复
    request_in_flight_.store(false);
    bool was_active = false;
    bool was_tts = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        was_active = response_active_;
        was_tts = tts_started_;
        response_active_ = false;
        tts_started_ = false;
        response_text_.clear();
    }
    if (adapter_ && was_active && cfg_.interrupt_enable) {
        adapter_->Interrupt();
    }
    if (was_tts) EmitTts("stop");
}

void DirectVoiceProtocol::SendStopListening() {
    // 用户说完，提交输入并触发模型响应（各服务商通过 CommitInput 实现）
    listening_.store(false);
    if (adapter_) {
        last_activity_us_.store(esp_timer_get_time());
        // quiet 模式不主动播报，但需要在提问提交前把最近一帧图像放进输入缓冲，
        // 这样模型才能在用户随后提问时"看到"画面。
        std::vector<uint8_t> frame;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const int64_t now = esp_timer_get_time();
            if (!quiet_frame_.empty() && now - quiet_frame_us_ < 10 * 1000 * 1000) {
                frame.swap(quiet_frame_);
            }
            quiet_frame_us_ = 0;
        }
        if (!frame.empty() && VisionModuleIsRunning()) {
            adapter_->SendImage(frame.data(), frame.size());
        }
        request_in_flight_.store(true);
        adapter_->CommitInput();
    }
}

void DirectVoiceProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    if (adapter_) adapter_->Interrupt();
    request_in_flight_.store(false);
    bool was_tts = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        was_tts = tts_started_;
        tts_started_ = false;
        response_active_ = false;
        response_text_.clear();
    }
    if (was_tts) EmitTts("stop");
}

void DirectVoiceProtocol::SendWakeWordDetected(const std::string& wake_word) {
    (void)wake_word;
}

bool DirectVoiceProtocol::SendText(const std::string& text) {
    // 文本模式（网页输入）转发为语音会话内的文本问题
    (void)text;
    return false;  // 当前服务商适配器以音频为主；文本输入为增强项
}

// 摄像头开关耗时长（初始化/下电可达数百毫秒~2 秒），放到独立任务执行，
// 且不捕获协议对象，避免协议重建时访问已释放内存。
static void VisionSwitchTask(void* arg) {
    auto* request = static_cast<std::pair<std::string, bool>*>(arg);
    const bool enable = request->second;
    VisionConfig vcfg = VisionConfigLoad();
    vcfg.enable = enable;
    VisionConfigSave(vcfg);
    bool ok = true;
    if (enable) {
        ok = VisionModuleStart();
        ESP_LOGI(TAG, "vision on via voice: %s -> %d", request->first.c_str(), ok);
    } else {
        VisionModuleStop();
        ESP_LOGI(TAG, "vision off via voice: %s", request->first.c_str());
    }
    delete request;

    // 需求 FR-5.2：摄像头状态变化给出屏幕反馈。
    Application::GetInstance().Schedule([enable, ok]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display == nullptr) return;
        if (enable && !ok) {
            display->ShowNotification("摄像头打开失败，请检查接线", 3000);
        } else {
            display->ShowNotification(enable ? "摄像头已打开" : "摄像头已关闭", 3000);
        }
    });
    vTaskDelete(nullptr);
}

static bool MatchSwitchPhrase(const std::string& text, const std::string& phrases, bool* enable) {
    auto list = SplitPhrases(phrases);
    std::string open_phrase = list.size() > 0 ? list[0] : std::string("打开摄像头");
    std::string close_phrase = list.size() > 1 ? list[1] : std::string("关闭摄像头");
    if (!open_phrase.empty() && text.find(open_phrase) != std::string::npos) {
        *enable = true;
        return true;
    }
    if (!close_phrase.empty() && text.find(close_phrase) != std::string::npos) {
        *enable = false;
        return true;
    }
    return false;
}

bool DirectVoiceProtocol::HandleVisionCommand(const std::string& text) {
    VisionConfig vcfg = VisionConfigLoad();
    if (vcfg.trigger_mode == "manual") {
        return false;
    }

    bool enable = false;
    if (MatchSwitchPhrase(text, vcfg.switch_phrases, &enable)) {
        auto* request = new (std::nothrow) std::pair<std::string, bool>(text, enable);
        if (request != nullptr &&
            xTaskCreate(VisionSwitchTask, "vision_switch", 6144, request, 4, nullptr) != pdPASS) {
            delete request;
            return false;
        }
        return request != nullptr;
    }

    for (const auto& phrase : SplitPhrases(vcfg.command_phrases)) {
        if (!phrase.empty() && text.find(phrase) != std::string::npos) {
            // 本函数运行在适配器接收线程，取帧/发送图像交给主任务，避免阻塞音频。
            Application::GetInstance().SnapAndAnalyze();
            return true;
        }
    }
    return false;
}

bool DirectVoiceProtocol::SnapAndAnalyze() {
    if (!adapter_ || !adapter_->IsConnected()) return false;
    if (!VisionModuleIsRunning() && !VisionModuleStart()) {
        ESP_LOGW(TAG, "snap failed: camera not available");
        return false;
    }
    size_t len = 0;
    uint8_t* jpeg = VisionModuleSnapshotJpeg(&len);
    if (!jpeg || len == 0) {
        free(jpeg);
        return false;
    }
    bool ok = adapter_->SendImage(jpeg, len);
    free(jpeg);
    if (ok) {
        last_activity_us_.store(esp_timer_get_time());
        request_in_flight_.store(true);
        adapter_->RequestResponse();
    }
    return ok;
}

bool DirectVoiceProtocol::IsVisionRunning() const {
    return VisionModuleIsRunning();
}

bool DirectVoiceProtocol::IsIdleTimedOut() const {
    if (!adapter_ || !adapter_->IsConnected()) return false;
    if (cfg_.session_timeout_s <= 0) return false;
    const int64_t last = last_activity_us_.load();
    if (last <= 0) return false;
    return (esp_timer_get_time() - last) >= (int64_t)cfg_.session_timeout_s * 1000 * 1000;
}

void DirectVoiceProtocol::CreateAdapter() {
    adapter_ = CreateVoiceAdapter(provider_);
}

void DirectVoiceProtocol::OnAdapterAudio(const uint8_t* pcm, size_t len, int sample_rate) {
    if (!on_incoming_audio_ || !pcm || len == 0) return;
    last_activity_us_.store(esp_timer_get_time());

    bool emit_start = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!tts_started_) {
            tts_started_ = true;
            response_active_ = true;
            emit_start = true;
        }
    }
    if (emit_start) EmitTts("start");

    auto packet = std::make_unique<AudioStreamPacket>();
    // 直连服务商下发原始 PCM16，由 AudioService 直接重采样播放。
    packet->opus = false;
    packet->sample_rate = sample_rate;
    packet->frame_duration = server_frame_duration_;
    packet->payload.assign(pcm, pcm + len);
    on_incoming_audio_(std::move(packet));
}

void DirectVoiceProtocol::OnAdapterText(const std::string& text) {
    if (text.empty()) return;
    last_activity_us_.store(esp_timer_get_time());
    bool emit_start = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        response_active_ = true;
        response_text_ += text;
        if (!tts_started_) {
            tts_started_ = true;
            emit_start = true;
        }
    }
    // 文本在回复结束时统一上屏，避免每个 delta 触发一次 Schedule。
    if (emit_start) EmitTts("start");
}

void DirectVoiceProtocol::OnUserText(const std::string& text) {
    ESP_LOGI(TAG, "user said: %s", text.c_str());
    last_activity_us_.store(esp_timer_get_time());
    if (on_incoming_json_) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "stt");
        cJSON_AddStringToObject(root, "text", text.c_str());
        on_incoming_json_(root);
        cJSON_Delete(root);
    }
    // 开关摄像头在独立任务中执行；单次拍照只做一次取帧+发送，开销可接受。
    HandleVisionCommand(text);
}

void DirectVoiceProtocol::OnAdapterState(const std::string& state) {
    ESP_LOGI(TAG, "adapter state: %s", state.c_str());
    if (state == "streaming") {
        if (on_audio_channel_opened_) on_audio_channel_opened_();
    } else if (state == "closed") {
        request_in_flight_.store(false);
        bool was_tts = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            was_tts = tts_started_;
            tts_started_ = false;
            response_active_ = false;
            response_text_.clear();
        }
        if (was_tts) EmitTts("stop");
        if (on_audio_channel_closed_) on_audio_channel_closed_();
    } else if (state == "response_done") {
        request_in_flight_.store(false);
        std::string text;
        bool was_tts = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            text = response_text_;
            was_tts = tts_started_;
            tts_started_ = false;
            response_active_ = false;
            response_text_.clear();
        }
        if (!text.empty()) {
            EmitTts("sentence_start", text);
        }
        if (was_tts) EmitTts("stop");
    }
}

void DirectVoiceProtocol::ResetVisionAnalysisState() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    periodic_frame_count_ = 0;
    last_event_sample_.clear();
    last_event_size_ = 0;
}

// event 模式的变化检测：按固定步长采样 JPEG 字节，与上一帧比较。
// 只保留采样指纹，不缓存整帧，避免持续占用 PSRAM。
bool DirectVoiceProtocol::FrameLooksChanged(const uint8_t* jpeg, size_t len) {
    constexpr size_t kSamples = 256;
    constexpr size_t kSampleStep = 97;  // 质数步长，避开 JPEG 头部固定字段
    constexpr size_t kAvgDiffThreshold = 16;
    constexpr size_t kSizeChangePercent = 15;

    std::vector<uint8_t> sample;
    sample.reserve(kSamples);
    for (size_t i = 0; i < kSamples && i * kSampleStep < len; ++i) {
        sample.push_back(jpeg[i * kSampleStep]);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    bool changed = false;
    if (!last_event_sample_.empty() && last_event_sample_.size() == sample.size()) {
        size_t diff = 0;
        for (size_t i = 0; i < sample.size(); ++i) {
            diff += sample[i] > last_event_sample_[i] ? (sample[i] - last_event_sample_[i])
                                                      : (last_event_sample_[i] - sample[i]);
        }
        const size_t avg_diff = diff / sample.size();
        const size_t size_delta = len > last_event_size_ ? len - last_event_size_
                                                         : last_event_size_ - len;
        changed = avg_diff >= kAvgDiffThreshold ||
                  size_delta * 100 >= last_event_size_ * kSizeChangePercent;
    }

    last_event_sample_ = std::move(sample);
    last_event_size_ = len;
    return changed;
}

void DirectVoiceProtocol::OnVisionFrame(const uint8_t* jpeg, size_t len) {
    if (!jpeg || len == 0) return;

    VisionConfig vcfg = VisionConfigLoad();
    if (!vcfg.continuous_enable) return;

    // 无论播报模式如何，都缓存最近一帧：用户随后提问（"现在看到什么"）时，
    // SendStopListening() 会把这一帧随本轮输入一起提交给模型。
    if (len <= 64 * 1024) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        quiet_frame_.assign(jpeg, jpeg + len);
        quiet_frame_us_ = esp_timer_get_time();
    }

    if (!adapter_ || !adapter_->IsConnected()) return;

    // 正在采集用户语音：此时 RequestResponse 会 commit 并截断用户这句话。
    // 已有回复在处理中时也不再发起新的请求。
    if (listening_.load() || request_in_flight_.load()) return;

    if (vcfg.announce_mode == "periodic") {
        // 需求 FR-5.3：periodic 每 N 帧主动播报一次摘要（默认 5 帧 ≈ 10 秒）。
        constexpr int kPeriodicAnnounceEvery = 5;
        bool due = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            due = (++periodic_frame_count_ >= kPeriodicAnnounceEvery);
            if (due) periodic_frame_count_ = 0;
        }
        if (!due) return;
    } else if (vcfg.announce_mode == "event") {
        // 仅在检测到画面显著变化时播报。
        if (!FrameLooksChanged(jpeg, len)) return;
    } else {
        // quiet：静默分析，只缓存，不主动请求回复。
        return;
    }

    if (!adapter_->SendImage(jpeg, len)) return;
    last_activity_us_.store(esp_timer_get_time());
    request_in_flight_.store(true);
    adapter_->RequestResponse();
}

void DirectVoiceProtocol::EmitTts(const char* state, const std::string& text) {
    if (!on_incoming_json_) return;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", state);
    if (!text.empty()) {
        cJSON_AddStringToObject(root, "text", text.c_str());
    }
    on_incoming_json_(root);
    cJSON_Delete(root);
}
