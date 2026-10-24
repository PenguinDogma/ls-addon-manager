#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "recorder_feature.h"
#include "recorder_service.h"
#include "../config/config_manager.h"
#include "../log/logger.h"
#include "addon/text_util.h"
#include "../../sdk/include/eam/version.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

namespace eam {
namespace features {
namespace recorder {

namespace {

ConfigManager& Cfg() { return ConfigManager::Instance(); }

Backends g_backends;
std::unique_ptr<rec::Service> g_service;
std::atomic<rec::Service*> g_live{ nullptr };   // what the hooks use (null while off)
std::thread g_poll;
std::atomic<bool> g_stop{ false };

// Every call into the service from outside (the dispatch observer, the addons' calls, the poll thread) counts itself in while it runs, so that Stop() frees the service only after the
// last of them has left it (a sleep was not enough: a frame being copied or a save being started can take longer).
std::atomic<int> g_inFlight{ 0 };
struct InService {
    rec::Service* service;
    InService() { g_inFlight.fetch_add(1); service = g_live.load(); }
    ~InService() { g_inFlight.fetch_sub(1); }
    InService(const InService&) = delete;
    InService& operator=(const InService&) = delete;
};

using nr::util::Wide;
using nr::util::Utf8;
using nr::util::JsonString;

// ---- the settings (config.json, section "Recorder")

rec::Settings Load() {
    rec::Settings s;
    s.on = Cfg().Get(kId, "keep", "1") == "1";   // the rolling buffer; off: the key records the next ten seconds
    s.shown = Cfg().Get(kId, "shown", "0") == "1";
    s.seconds = std::clamp(static_cast<float>(std::atof(Cfg().Get(kId, "seconds", "15").c_str())), 1.0f, 60.0f);
    s.budgetMb = std::clamp(std::atoi(Cfg().Get(kId, "budget_mb", "8192").c_str()), 256, 65536);
    s.folder = Wide(Cfg().Get(kId, "folder", ""));
    s.hotkeyOn = Cfg().Get(kId, "hotkey", "1") == "1";
    s.key = std::atoi(Cfg().Get(kId, "key", "112").c_str());   // 112 = VK_F1
    s.afterSeconds = std::clamp(static_cast<float>(std::atof(Cfg().Get(kId, "after_seconds", "0").c_str())), 0.0f, 60.0f);
    s.compact = Cfg().Get(kId, "compact", "1") == "1";
    s.beep = Cfg().Get(kId, "beep", "1") == "1";
    s.note = Cfg().Get(kId, "note", "1") == "1";
    s.saveAfter = std::max(0, std::atoi(Cfg().Get(kId, "test_save_after", "0").c_str()));   // tests only
    return s;
}

void Store(const rec::Settings& s) {
    Cfg().Set(kId, "keep", s.on ? "1" : "0");
    Cfg().Set(kId, "shown", s.shown ? "1" : "0");
    Cfg().Set(kId, "seconds", std::to_string(static_cast<int>(s.seconds + 0.5f)));
    Cfg().Set(kId, "budget_mb", std::to_string(s.budgetMb));
    Cfg().Set(kId, "folder", Utf8(s.folder));
    Cfg().Set(kId, "hotkey", s.hotkeyOn ? "1" : "0");
    Cfg().Set(kId, "key", std::to_string(s.key));
    Cfg().Set(kId, "after_seconds", std::to_string(static_cast<int>(s.afterSeconds + 0.5f)));
    Cfg().Set(kId, "compact", s.compact ? "1" : "0");
    Cfg().Set(kId, "beep", s.beep ? "1" : "0");
    Cfg().Set(kId, "note", s.note ? "1" : "0");
    Cfg().Save();
}

// ---- what the hooks do

void Observe(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {
    InService in;
    if (in.service) in.service->OnDispatch(ctx, x, y, z);
}
uint32_t FlagsFn() {
    InService in;
    return in.service ? in.service->Flags() : 0u;
}
void OfferFn(void* ctx, void* texture, uint32_t source, uint32_t content, uint32_t tag, uint32_t cropW, uint32_t cropH) {
    InService in;
    if (in.service) in.service->Offer(static_cast<ID3D11DeviceContext*>(ctx), static_cast<ID3D11Texture2D*>(texture), source, content, tag, cropW, cropH);
}

// What the note beside a saved file says about the manager and the addons: which are on and the few settings that decide how a picture comes out.
std::string Info() {
    static const char* const ids[] = { "DLSS5NR01", "DLSS4DLAA", "FSR3UPSC", "XESSUPSC", "ECHOFG", "VSRUPSC" };
    static const char* const keys[] = { "modelPreset", "passes", "workingScale", "autoQuality", "intensity", "style", "dlaaPreset", "sharpen", "multiplier", "own" };
    std::string out = std::string("\"manager_version\": ") + JsonString(EAM_VERSION_STRING) + ",\n  \"addons\": {";
    bool firstAddon = true;
    for (const char* id : ids) {
        out += firstAddon ? "\n" : ",\n"; firstAddon = false;
        out += std::string("    ") + JsonString(id) + ": { \"enabled\": " + (Cfg().IsAddonEnabled(id, false) ? "true" : "false");
        for (const char* key : keys) {
            const std::string v = Cfg().Get(id, key, "");
            if (!v.empty()) out += std::string(", ") + JsonString(key) + ": " + JsonString(v);
        }
        out += " }";
    }
    out += "\n  }";
    return out;
}

int SaveFn(const char* name, bool clear) {
    InService in;
    return in.service ? in.service->SaveNamed(name ? name : "", clear) : 0;
}

void PollLoop() {
    while (!g_stop.load()) {
        { InService in; if (in.service) in.service->Poll(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void ApplyFromConfig() { if (g_service) g_service->Configure(Load()); }

} // namespace

void SetBackends(const Backends& backends) { g_backends = backends; }

void Start() {
    if (g_service) return;
    g_service = std::make_unique<rec::Service>();
    g_service->SetLog([](const char* m) { LOG_INFO("Recorder", "%s", m); });
    g_service->SetInfoProvider(&Info);
    ApplyFromConfig();
    if (g_backends.publishSaved) g_service->SetSavedCallback([](const std::wstring& path) { g_backends.publishSaved(path.c_str()); });
    g_stop = false;
    g_live.store(g_service.get(), std::memory_order_release);
    g_poll = std::thread(PollLoop);
    if (g_backends.setHostHooks) g_backends.setHostHooks(&FlagsFn, &OfferFn, &SaveFn);
    if (g_backends.setDispatchObserver) g_backends.setDispatchObserver(&Observe);
    LOG_INFO("Recorder", "on");
}

void Stop() {
    if (!g_service) return;
    if (g_backends.setDispatchObserver) g_backends.setDispatchObserver(nullptr);
    if (g_backends.setHostHooks) g_backends.setHostHooks(nullptr, nullptr, nullptr);
    g_live.store(nullptr, std::memory_order_release);
    g_stop = true;
    if (g_poll.joinable()) g_poll.join();
    for (int i = 0; i < 3000 && g_inFlight.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // the callers that had the pointer leave
    if (g_inFlight.load() > 0 || g_service->Faulted()) {   // still inside after three seconds, or a fault left locks held: freeing it could crash Lossless Scaling, leaking it cannot
        LOG_WARN("Recorder", "off; the recorder's memory is left alone (%s)", g_service->Faulted() ? "after a fault" : "a call is still inside it");
        g_service.release();
        return;
    }
    g_service->Shutdown();
    g_service.reset();
    LOG_INFO("Recorder", "off");
}

std::string Status() { return g_service ? g_service->Summary() : std::string(); }

void RenderOptions() {
    if (!g_service) return;
    rec::Settings s = g_service->Current();
    bool changed = false;

    if (ImGui::Checkbox("保留最近几秒以备保存", &s.on)) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("开启：录制器始终在内存里保留最近几秒，按快捷键立即保存。\n"
                          "关闭：什么都不保留；快捷键会录制接下来的十秒，存完即停（内存也只为这十秒占用）。");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    float seconds = s.seconds; if (ImGui::SliderFloat("保留秒数", &seconds, 1.0f, 60.0f, "%.0f 秒")) { s.seconds = seconds; changed = true; }
    {
        int mode = s.compact ? 0 : 1; const char* modes[] = { "紧凑（8 位画面，体积小）", "原样（按捕获存储，HDR 下非常大）" };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::Combo("帧", &mode, modes, 2)) { s.compact = mode == 0; changed = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("紧凑：HDR 帧先在显卡上转成 8 位 SDR 画面（即你看到的画面；高于 SDR 白位的高光会被削平）：体积约为六分之一，10 秒片段约 1 GB，几秒即可存完。\n"
                              "原样：每一帧都按 Lossless Scaling 捕获到的样子保存，包括半精度浮点 HDR：2560x1440 下每秒约 0.85 GB，写入需一分钟。用于依赖 HDR 数值的缺陷。\n"
                              "两种模式下 8 位帧都原样保留。更改此选项会让录制重新开始。");
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    int budget = s.budgetMb; if (ImGui::SliderInt("为其保留的内存", &budget, 512, 32768, "%d MB")) { s.budgetMb = budget; changed = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("存满后最早的帧先被丢弃。即便经过压缩，4K HDR 的一秒也可能占到 200 MB 以上：在 4K 下录长片段请调高此值。");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    float after = s.afterSeconds; if (ImGui::SliderFloat("按键后继续录制", &after, 0.0f, 30.0f, after < 0.5f ? "立即保存" : "%.0f 秒")) { s.afterSeconds = after; changed = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("按下快捷键后，再继续录制这么多秒然后保存：文件既包含按键之前的几秒，也包含之后的这几秒。");
    if (ImGui::Checkbox("录制显示的画面", &s.shown)) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("录制 Lossless Scaling 显示出来的画面（插件会提供它们呈现的帧），而不是它从游戏捕获的帧。\n"
                          "需要开启一个能提供这些帧的插件（神经渲染、各上采样器、EchoFG）。用于查看你所见画面中的闪烁与拖影。");

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("快捷键");
    ImGui::SameLine();
    if (ImGui::Checkbox("##hotkey_on", &s.hotkeyOn)) changed = true;
    ImGui::SameLine();
    ImGui::TextDisabled("Ctrl+Shift +");
    ImGui::SameLine();
    char name[16]; snprintf(name, sizeof name, "F%d", s.key - 0x70 + 1);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    if (ImGui::BeginCombo("##recorder_key", name)) {
        for (int i = 0; i < 12; ++i) {
            char n[8]; snprintf(n, sizeof n, "F%d", i + 1);
            if (ImGui::Selectable(n, s.key == 0x70 + i)) { s.key = 0x70 + i; changed = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("在游戏拥有焦点时按下。OBS 的回放缓冲快捷键也可以设成同一个：一次按下即可两者都保存。");

    if (ImGui::Checkbox("声音（按下按键时一声短响，保存时上行双响，无内容可存时低音）", &s.beep)) changed = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("测试声音")) g_service->TestSound();
    ImGui::SameLine();
    if (ImGui::Checkbox("同时写入说明文件", &s.note)) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("<文件>.lsrec.json：记录游戏、画面、当时开启了哪些插件及其主要设置，让每个片段都能说明自己是如何录制的。");

    static char folder[512] = {};
    static bool folderRead = false;
    if (!folderRead) { snprintf(folder, sizeof folder, "%s", Utf8(s.folder).c_str()); folderRead = true; }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
    ImGui::InputTextWithHint("##recorder_folder", "文件夹（留空：Videos\\Lossless Scaling）", folder, sizeof folder);
    if (ImGui::IsItemDeactivatedAfterEdit()) { s.folder = Wide(folder); changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("打开")) {
        std::wstring dir = s.folder;
        if (dir.empty()) { PWSTR v = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &v)) && v) { dir = std::wstring(v) + L"\\Lossless Scaling"; CoTaskMemFree(v); } }
        CreateDirectoryW(dir.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    if (ImGui::Button(g_service->Clipping() ? "正在录制片段…" : g_service->Pending() ? "即将保存…" : s.on ? "保存最近几秒" : "录制片段")) g_service->Request();
    ImGui::SameLine();
    const std::string summary = g_service->Summary();
    ImGui::TextDisabled("%s", summary.empty() ? "空闲" : summary.c_str());
    bool ok = true; const std::string last = g_service->LastResult(ok);
    if (!last.empty()) ImGui::TextColored(ok ? ImVec4(0.7f, 0.9f, 0.5f, 1.0f) : ImVec4(0.95f, 0.65f, 0.2f, 1.0f), "%s", last.c_str());
    ImGui::TextDisabled("它会自行寻找 Lossless Scaling 捕获的帧，帧生成开启或关闭均可（关闭时：取其缩放处理趟读取的那一帧）；无需开启任何插件。");

    if (changed) { Store(s); g_service->Configure(s); }
}

} // namespace recorder
} // namespace features
} // namespace eam
