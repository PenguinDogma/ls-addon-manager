#include "superlog.h"
#include "../config/config_manager.h"
#include "../diag/history.h"
#include "../diag/report.h"
#include "../log/logger.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace eam {
namespace features {
namespace superlog {

namespace {

ConfigManager& Cfg() { return ConfigManager::Instance(); }

std::atomic<bool> g_on{ false };
std::thread g_poll;
std::atomic<bool> g_stop{ false };
std::atomic<bool> g_busy{ false };
std::atomic<uint64_t> g_lastAutoMs{ 0 };
std::mutex g_textMutex; std::string g_lastText; uint64_t g_lastTextAt = 0; fs::path g_lastZip;

bool Flag(const char* key, bool def) { return Cfg().Get(kId, key, def ? "1" : "0") == "1"; }

fs::path LsDir() {   // beside Lossless.dll (Lossless Scaling's own folder)
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&LsDir), &self);
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH * 2);
    return n ? fs::path(std::wstring(path, n)).parent_path() : fs::path(L".");
}

void SaveReport(const std::string& reason) {
    if (g_busy.exchange(true)) return;   // one at a time
    const fs::path ls = LsDir(), out = ls / L"reports";
    const DiagResult d = MakeReport(ls, reason, out);
    {   // the newest ten stay
        std::error_code ec; std::vector<fs::path> zips;
        for (const auto& e : fs::directory_iterator(out, ec)) if (e.is_regular_file() && e.path().filename().wstring().rfind(L"LSAddonManager-report-", 0) == 0) zips.push_back(e.path());
        std::sort(zips.begin(), zips.end());
        for (size_t i = 0; i + 10 < zips.size(); ++i) fs::remove(zips[i], ec);
    }
    { std::lock_guard<std::mutex> lock(g_textMutex); g_lastText = d.ok ? "报告已保存：" + d.zip.filename().string() : "报告无法保存：" + d.message; g_lastTextAt = GetTickCount64(); if (d.ok) g_lastZip = d.zip; }
    if (d.ok) LOG_INFO("SuperLog", "report saved (%s): %ls", reason.c_str(), d.zip.c_str()); else LOG_WARN("SuperLog", "the report could not be saved: %s", d.message.c_str());
    g_busy = false;
}

void PollLoop() {
    bool was = false;
    while (!g_stop) {
        const bool down = Flag("hotkey", true) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F12) & 0x8000);
        if (down && !was) { history::Add("hotkey", "Ctrl+Shift+F12: a report is saved"); LOG_INFO("SuperLog", "hotkey: saving a report"); SaveReport("the hotkey"); Beep(1200, 60); }
        was = down;
        Sleep(50);
    }
}

LogLevel ConfiguredLevel() { return static_cast<LogLevel>(std::clamp(Cfg().GlobalGetOr<int>(nullptr, "log_level", 2), 0, 4)); }

} // namespace

void Start() {
    if (g_on.exchange(true)) return;
    if (Flag("detail", true)) Logger::Instance().SetMinLevel(LogLevel::Debug);   // (not saved: switching the feature off puts the setting's own level back)
    g_stop = false; g_poll = std::thread(PollLoop);
    LOG_INFO("SuperLog", "on: the log is at Debug%s; Ctrl+Shift+F12 saves a report", Flag("auto", true) ? ", and a report is saved by itself when an addon reports an error" : "");
}

void Stop() {
    if (!g_on.exchange(false)) return;
    g_stop = true; if (g_poll.joinable()) g_poll.join();
    Logger::Instance().SetMinLevel(ConfiguredLevel());
    LOG_INFO("SuperLog", "off");
}

std::string Status() {
    if (!g_on) return {};
    std::lock_guard<std::mutex> lock(g_textMutex);
    if (!g_lastText.empty() && GetTickCount64() - g_lastTextAt < 60000) return g_lastText;
    return "就绪：Ctrl+Shift+F12 可保存一份报告";
}

void NotifyProblem(const std::string& addon, const std::string& text) {
    history::Add("error", addon + ": " + text);   // (always: the report that follows later has it)
    if (!g_on || !Flag("auto", true)) return;
    const uint64_t now = GetTickCount64(), last = g_lastAutoMs.load();
    if (last && now - last < 5 * 60 * 1000) return;
    g_lastAutoMs = now;
    const std::string reason = "an addon reported an error: " + addon + ": " + text;
    std::thread([reason] { Sleep(3000); SaveReport(reason); }).detach();   // (a few seconds on, so the log has what follows the error)
}

void RenderOptions() {
    bool detail = Flag("detail", true), autoSave = Flag("auto", true), hotkey = Flag("hotkey", true);
    if (ImGui::Checkbox("本功能开启期间记录详细日志（Debug）", &detail)) { Cfg().Set(kId, "detail", detail ? "1" : "0"); Cfg().Save(); Logger::Instance().SetMinLevel(detail ? LogLevel::Debug : ConfiguredLevel()); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("日志会记录 Lossless Scaling 请求的每个着色器、每个处理趟等更多内容。关闭本功能后，“设置 > 日志详细度”会恢复原状。");
    if (ImGui::Checkbox("插件报告错误时自动保存报告", &autoSave)) { Cfg().Set(kId, "auto", autoSave ? "1" : "0"); Cfg().Save(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("当插件的状态行变红（\"DLSS 无法运行\"）时，几秒后会保存一份报告，让日志留下随后的内容。每五分钟最多一次。");
    if (ImGui::Checkbox("Ctrl+Shift+F12 保存报告", &hotkey)) { Cfg().Set(kId, "hotkey", hotkey ? "1" : "0"); Cfg().Save(); }
    if (ImGui::Button("立即保存报告")) std::thread([] { SaveReport("the button"); }).detach();
    ImGui::SameLine();
    if (ImGui::Button("打开 reports 文件夹")) { std::error_code ec; fs::create_directories(LsDir() / L"reports", ec); ShellExecuteW(nullptr, L"open", (LsDir() / L"reports").c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    { std::lock_guard<std::mutex> lock(g_textMutex); if (!g_lastText.empty()) ImGui::TextDisabled("%s", g_lastText.c_str()); }
    ImGui::TextDisabled("报告内含日志、设置、系统信息（显卡、驱动、显示器、版本）、Lossless Scaling 窗口发送的内容与时间、各插件发布的每条序列最近十分钟的数据，以及每个插件的帧跟踪记录。任何内容都不会上传：文件由你自己发送。");
}

} // namespace superlog
} // namespace features
} // namespace eam
