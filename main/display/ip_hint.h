#pragma once

#include <string>

// 配置网页地址引导（需求 FR-1.4 / FR-1.5）
//
// 设备拿到 IP 后，空闲态在屏幕上滚动显示「设置页 http://<设备IP>」，用户照着
// 输入即可打开板载配置网页。退出条件：网页被访问 / 保存并重启 / 唤醒或进入
// 对话 / 超时（10 分钟）。无屏或屏幕初始化失败时所有调用都是安全的空操作。
namespace IpHint {

// 开始引导（网络连接成功后调用；实际 URL 在 Tick 中按 STA/AP 状态解析）。
void Show();

// 结束引导。reason 仅用于日志。
void Hide(const char* reason);

bool IsActive();

// 主循环每秒调用：刷新 IP（DHCP 变化）、检查超时、对话开始时自动退出。
void Tick();

// 屏幕被其他逻辑清理后（例如回到 IDLE）立即恢复引导文本。
void Redraw();

// 当前引导地址，例如 "http://192.168.3.33"；未解析到时为空。
std::string Url();

// 记录「配置页已被访问过」。持久化到 NVS：完成设置后的后续启动不再占屏
// （需求 FR-1.5 退出条件 b）。恢复默认配置会清除该标记。
void MarkConfigured();

}  // namespace IpHint
