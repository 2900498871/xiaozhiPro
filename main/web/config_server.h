#pragma once

#include <esp_http_server.h>

class DirectVoiceProtocol;

// 板载配置网页服务：http://<设备IP> 打开配置页
// API：/api/status /api/config /api/test /api/session /api/camera（详见需求文档 §3）
bool ConfigServerInit(DirectVoiceProtocol* protocol);
void ConfigServerDeinit();
