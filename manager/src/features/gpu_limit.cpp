#include "gpu_limit.h"
#include "../config/config_manager.h"
#include "../log/logger.h"
#include "imgui.h"
#include <algorithm>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace eam {
namespace features {
namespace gpulimit {

namespace {

ConfigManager& Cfg() { return ConfigManager::Instance(); }
Backends g_backends;
std::atomic<bool> g_on{ false };

uint32_t Percent() { return static_cast<uint32_t>(std::clamp(std::atoi(Cfg().Get(kId, "percent", "95").c_str()), 80, 100)); }
uint32_t Load() {   // read at most once a second (the status line asks every frame the window is shown)
    static uint64_t at = 0; static uint32_t last = 0;
    const uint64_t now = GetTickCount64();
    if (now - at >= 1000) { at = now; last = g_backends.readLoad ? g_backends.readLoad() : 0u; }
    return last;
}

} // namespace

void SetBackends(const Backends& backends) { g_backends = backends; }

void Start() {
    g_on = true;
    if (g_backends.setLimit) g_backends.setLimit(Percent());
    LOG_INFO("GpuLimit", "on: keep the card under %u %%", Percent());
}

void Stop() {
    if (!g_on.exchange(false)) return;
    if (g_backends.setLimit) g_backends.setLimit(0);
    LOG_INFO("GpuLimit", "off");
}

std::string Status() {
    if (!g_on) return {};
    const uint32_t limit = Percent(), load = Load();
    char text[120];
    if (!load) snprintf(text, sizeof text, "限制 %u %%", limit);
    else snprintf(text, sizeof text, "限制 %u %%：显卡当前 %u %%%s", limit, load, load >= limit ? "（超限：插件正在降低自身开销）" : "");
    return text;
}

void RenderOptions() {
    int percent = static_cast<int>(Percent());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::SliderInt("显卡负载保持在以下", &percent, 80, 100, "%d %%")) {
        Cfg().Set(kId, "percent", std::to_string(percent));
        Cfg().Save();
        if (g_backends.setLimit && g_on) g_backends.setLimit(static_cast<uint32_t>(percent));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("显卡负载连续几秒达到或超过此值时，各插件会降低自身的开销（神经渲染会缩小模型的工作尺寸；各上采样器的\"显卡吃满时用更轻的上采样\"会启动），"
                          "负载明显低于此值后再恢复原状。100 % 表示仅在显卡完全吃满时才生效。它与插件自身的设置配合工作：需要开启神经渲染的自动画质。");
    const uint32_t load = Load();
    if (load) ImGui::TextDisabled("显卡当前负载 %u %%。%s", load, load >= static_cast<uint32_t>(percent) ? "已超过限制" : "");
    else ImGui::TextDisabled("显卡负载在游戏被缩放时读取（否则读数为空闲）。");
    ImGui::TextDisabled("它读取的是 Lossless Scaling 所运行的那块显卡；无需开启任何插件，但只对已开启的插件生效。");
}

} // namespace gpulimit
} // namespace features
} // namespace eam
