#include "tab_settings.h"
#include "../gui_scale.h"
#include "../../addon/addon_manager.h"
#include "../../config/config_manager.h"
#include "../../config/settings_backup.h"
#include "../../diag/diagnostics.h"
#include "../../host/gpu_stats.h"
#include "../../log/logger.h"
#include "../../update/update_check.h"
#include "../../../sdk/include/eam/version.h"
#include "../widgets/file_dialog.h"
#include "../widgets/toast.h"
#include "../widgets/tooltip.h"
#include "../gui_manager.h"
#include "imgui.h"
#include "eam/widgets.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace eam {

namespace fs = std::filesystem;

static void OpenPath(const std::wstring& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

static std::wstring ExeDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(buffer).parent_path().wstring();
}

static std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s((size_t)WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], (int)s.size(), nullptr, nullptr);
    return s;
}

static std::string WindowsVersion() {
    using RtlGetVersion_t = LONG (WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{ sizeof v };
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto f = (RtlGetVersion_t)GetProcAddress(nt, "RtlGetVersion")) f(&v);
    char b[64]; snprintf(b, sizeof b, "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return b;
}

// What goes into info.txt of the diagnostics zip: versions, the machine, and every addon's state. No personal data beyond the folder
// path of the install.
static std::string BuildDiagnosticsSummary(AddonManager* manager) {
    std::ostringstream o;
    o << EAM_PRODUCT_NAME << " diagnostics\n";
    o << "manager: " << EAM_VERSION_STRING << " (built " << __DATE__ << " " << __TIME__ << ")\n";
    o << "system: " << WindowsVersion() << "\n";
    o << "install folder: " << Utf8(ExeDir()) << "\n";
    GpuStats::Instance().SampleOnce();
    const GpuStats::Snapshot g = GpuStats::Instance().Get();
    if (g.ok) o << "gpu: " << g.name << ", driver " << g.driver << ", " << g.vramTotalMB << " MB, power limit " << (int)g.powerLimitW << " W\n";
    else o << "gpu: not available (" << g.why << ")\n";
    o << "\naddons:\n";
    if (manager) for (const auto& a : manager->GetAddons()) {
        o << "  " << a.GetDisplayName() << " [" << a.id << "] v" << a.GetDisplayVersion()
          << "  enabled=" << (a.enabled ? "yes" : "no") << "  loaded=" << (a.IsLoaded() ? "yes" : "no") << "  faulted=" << (a.faulted ? "yes" : "no")
          << "  restart-needed=" << (a.RequiresRestart() ? "yes" : "no");
        if (!a.errorMessage.empty()) o << "  error: " << a.errorMessage;
        o << "\n";
    }
    auto& cfg = ConfigManager::Instance();
    o << "\nmanager settings: security_level=" << cfg.GlobalGetOr<int>(nullptr, "security_level", 0) << " log_level=" << cfg.GlobalGetOr<int>(nullptr, "log_level", 2)
      << " auto_load=" << cfg.GlobalGetOr<bool>(nullptr, "auto_load", true) << " interface_size=" << cfg.GlobalGetOr<int>("ui", "scale_percent", 100) << "%\n";
    return o.str();
}

static std::wstring DesktopDir() {
    PWSTR p = nullptr; std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) { out = p; CoTaskMemFree(p); }
    if (out.empty()) out = ExeDir();
    return out;
}

static std::wstring s_importPath;
static std::string s_importText;   // a settings file waiting for confirmation
static int s_importAddons = 0;
static bool s_askImport = false;

// Every option here takes effect as soon as it is changed and is saved at once.
void RenderTabSettings(AddonManager* manager) {
    auto& config = ConfigManager::Instance();
    ImGui::Dummy(ImVec2(0, S(4)));

    // ---- Backup and restore
    eam::ui::SectionLabel("备份与还原（保存和载入你的设置）");
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("把本管理器和所有插件的设置保存到一个文件，之后可以载入回来，也可用于其他电脑。插件各自保存的外观同样包含在该文件中。");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
    if (eam::ui::Button("保存设置到文件", eam::ui::icons::kSave, eam::ui::ButtonKind::Primary)) {
        std::wstring path;
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t name[64]; swprintf(name, 64, L"LSAddonManager-settings-%04d%02d%02d.json", t.wYear, t.wMonth, t.wDay);
        if (widgets::PickSaveFile(L"保存设置", name, L"设置备份", L"*.json", path)) {
            std::ofstream out(fs::path(path), std::ios::binary);
            out << MakeSettingsBackupText(config.Snapshot(), EAM_VERSION_STRING);
            out.close();
            widgets::ToastShow(out ? "设置已保存。" : "无法写入该文件。", out ? widgets::ToastType::Success : widgets::ToastType::Error);
        }
    }
    widgets::Tip("把所有插件和本管理器的设置保存到一个文件，可留作备份或迁移到其他电脑。");
    ImGui::SameLine();
    if (eam::ui::Button("从文件载入设置", eam::ui::icons::kDownload)) {
        std::wstring path;
        if (widgets::PickOpenFile(L"载入设置", L"设置备份", L"*.json", path)) {
            std::ifstream in(fs::path(path), std::ios::binary);
            std::stringstream ss; ss << in.rdbuf();
            const std::string text = ss.str();
            const BackupParse p = ParseSettingsBackup(text);
            if (!p.ok) widgets::ToastShow(p.message, widgets::ToastType::Error, 6.0f);
            else { s_importPath = path; s_importText = text; s_importAddons = p.addonCount; s_askImport = true; }
        }
    }
    widgets::Tip("用之前保存的文件中的设置替换当前设置。会先询问你，且当前的设置会另行保留。重启 Lossless Scaling 后生效。");
    if (config.RestartPending()) {
        ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn));
        ImGui::TextWrapped("已从文件载入设置。重启 Lossless Scaling 后即可使用。在那之前，你在此处或插件中所做的更改都不会保存，因此不会覆盖已载入的设置。");
        ImGui::PopStyleColor();
    }
    if (s_askImport) { ImGui::OpenPopup("载入设置"); s_askImport = false; }
    if (ImGui::BeginPopupModal("载入设置", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextWrapped("用此文件中的设置替换你当前的设置？");
        ImGui::TextDisabled("%s", Utf8(fs::path(s_importPath).filename().wstring()).c_str());
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::TextWrapped("它保存着 %d 个插件%s的设置。你当前的设置会先备份到配置文件旁，新设置在重启 Lossless Scaling 后生效。",
                           s_importAddons, s_importAddons == 1 ? "" : "");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(6)));
        if (eam::ui::Button("载入", eam::ui::icons::kCheck, eam::ui::ButtonKind::Primary)) {
            const ImportResult r = ImportSettings(s_importText, config.Snapshot(), fs::path(ExeDir()) / L"backups", [&](const nlohmann::json& j) { config.Replace(j, /*untilRestart=*/true); });
            widgets::ToastShow(r.message, r.ok ? widgets::ToastType::Success : widgets::ToastType::Error, 9.0f);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (eam::ui::Button("取消", eam::ui::icons::kClose)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- Interface
    ImGui::Dummy(ImVec2(0, S(16)));
    eam::ui::SectionLabel("界面");
    ImGui::Dummy(ImVec2(0, S(2)));
    {
        int pct = config.GlobalGetOr<int>("ui", "scale_percent", 100);
        const int def = 100;
        ImGui::SetNextItemWidth(S(320));
        if (eam::ui::SliderInt("界面大小", &pct, 75, 200, "%d%%", 0, &def)) {
            config.GlobalSet("ui", "scale_percent", pct);
            config.Save();
            GuiManager::RequestUserScale();   // applied at the start of the next frame
        }
        widgets::Tip("在 Windows 自身的显示缩放之上，放大或缩小此窗口中的所有内容。100% 即 Windows 要求的大小。");
    }
    bool openOnStart = config.GlobalGetOr<bool>("ui", "open_on_start", true);
    if (ImGui::Checkbox("Lossless Scaling 启动时打开此窗口", &openOnStart)) { config.GlobalSet("ui", "open_on_start", openOnStart); config.Save(); }
    widgets::Tip("关闭时，管理器会隐藏在通知区域（托盘）中启动。无论开关与否，插件都会照常加载运行。用 X 关闭此窗口也只是将其隐藏。");
    bool hotkeyOn = config.GlobalGetOr<bool>("ui", "hotkey_enabled", true);
    if (ImGui::Checkbox("打开/关闭快捷键：Ctrl+Shift +", &hotkeyOn)) { config.GlobalSet("ui", "hotkey_enabled", hotkeyOn); config.Save(); GuiManager::ApplyHotkey(); }
    widgets::Tip("在 Windows 中任何地方都有效，包括游戏占用焦点时。如果其他程序已占用该组合键，下方状态会提示，换一个按键即可。");
    ImGui::SameLine();
    {
        int idx = config.GlobalGetOr<int>("ui", "hotkey_vk", VK_F12) - VK_F1;
        if (idx < 0 || idx > 11) idx = 11;
        const char* names[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
        const ImGuiStyle& st = ImGui::GetStyle();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("F12").x + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x);
        if (ImGui::Combo("##mgrhotkey", &idx, names, 12)) { config.GlobalSet("ui", "hotkey_vk", VK_F1 + idx); config.Save(); GuiManager::ApplyHotkey(); }
    }
    ImGui::TextDisabled("快捷键：%s", GuiManager::HotkeyStatus());

    // ---- Addons
    ImGui::Dummy(ImVec2(0, S(16)));
    eam::ui::SectionLabel("插件");
    ImGui::Dummy(ImVec2(0, S(2)));
    {
        int securityLevel = config.GlobalGetOr<int>(nullptr, "security_level", 0);
        const char* securityItems[] = { "允许所有插件", "对不受信的插件给出警告", "阻止不受信的插件" };
        ImGui::SetNextItemWidth(S(320));
        if (ImGui::Combo("安全性", &securityLevel, securityItems, 3)) {
            config.GlobalSet(nullptr, "security_level", securityLevel); config.Save();
            widgets::ToastShow("在下次加载插件时生效", widgets::ToastType::Info);
        }
        widgets::Tip("如何处理 DLL 不在受信列表（addons\\trusted_addons.json）中的插件：允许、记录警告或拒绝加载。若该文件不存在，选择“阻止”会拒绝所有插件。");
    }
    bool autoLoad = config.GlobalGetOr<bool>(nullptr, "auto_load", true);
    if (ImGui::Checkbox("Lossless Scaling 启动时加载已启用的插件", &autoLoad)) { config.GlobalSet(nullptr, "auto_load", autoLoad); config.Save(); }
    widgets::Tip("关闭时不加载任何插件，需要时在目标插件上按“立即加载”即可。");

    // ---- Updates
    ImGui::Dummy(ImVec2(0, S(16)));
    eam::ui::SectionLabel("更新");
    ImGui::Dummy(ImVec2(0, S(2)));
    {
        bool daily = config.GlobalGetOr<bool>("updates", "check", true);
        if (ImGui::Checkbox("每天检查一次新版本", &daily)) { config.GlobalSet("updates", "check", daily); config.Save(); }
        widgets::Tip("默认开启。关闭后，管理器不会再自行联网；“立即检查”按钮仍可随时使用。");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("会向 github.com 查询本项目的最新发布版本，并与你当前的版本号比较。和任何下载一样，这会发送你的 IP 地址"
                           "以及程序名称和版本。不会下载或安装任何内容，这也是管理器唯一会通过互联网发送的信息。");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(4)));
        const update::Status st = update::Current();
        const bool checking = st.state == update::State::Checking;
        if (checking) ImGui::BeginDisabled();
        if (eam::ui::Button(checking ? "检查中…" : "立即检查", eam::ui::icons::kCheck)) update::StartCheckAsync();
        if (checking) ImGui::EndDisabled();
        widgets::Tip("无论上方设置如何，立即向 GitHub 查询。");
        if (st.state == update::State::Available) {
            ImGui::SameLine();
            if (eam::ui::Button("打开下载页面", eam::ui::icons::kExternal, eam::ui::ButtonKind::Primary)) ShellExecuteA(nullptr, "open", st.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            widgets::Tip("在浏览器中打开本项目的发布页面，由你自行下载并安装。");
        }
        const std::string line = update::DescribeStatus(st);
        ImGui::PushStyleColor(ImGuiCol_Text, st.state == update::State::Available ? eam::ui::theme::V(eam::ui::theme::kAccent) : (st.state == update::State::Failed ? eam::ui::theme::V(eam::ui::theme::kWarn) : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]));
        ImGui::TextWrapped("%s", line.c_str());
        ImGui::PopStyleColor();
    }

    // ---- Logging and support
    ImGui::Dummy(ImVec2(0, S(16)));
    eam::ui::SectionLabel("日志与支持");
    ImGui::Dummy(ImVec2(0, S(2)));
    {
        int logLevel = config.GlobalGetOr<int>(nullptr, "log_level", 2);
        const char* logItems[] = { "跟踪", "调试", "信息", "警告", "错误" };
        ImGui::SetNextItemWidth(S(320));
        if (ImGui::Combo("日志详细度", &logLevel, logItems, 5)) { config.GlobalSet(nullptr, "log_level", logLevel); config.Save(); Logger::Instance().SetMinLevel(static_cast<LogLevel>(logLevel)); }
        widgets::Tip("低于该级别的消息在记录前即被丢弃，包括日志页和 LSAddonManager.log 中的记录。“信息”是合适的默认值；“调试”和“跟踪”用于排查问题。");
    }
    if (eam::ui::Button("创建诊断文件", eam::ui::icons::kPackage, eam::ui::ButtonKind::Primary)) {
        const DiagResult d = CreateDiagnosticsZip(fs::path(ExeDir()), BuildDiagnosticsSummary(manager), fs::path(DesktopDir()));
        widgets::ToastShow(d.message, d.ok ? widgets::ToastType::Success : widgets::ToastType::Error, 9.0f);
        if (d.ok) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + d.zip.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    }
    widgets::Tip("把日志、设置和一份简短摘要（版本、显卡、已启用的插件）打包成一个 zip 放到桌面，用于提交问题报告。不会上传任何内容：文件交给谁由你决定。");
    ImGui::SameLine();
    if (eam::ui::Button("日志文件夹", eam::ui::icons::kFolder)) OpenPath(ExeDir() + L"\\logs");
    ImGui::SameLine();
    if (eam::ui::Button("插件文件夹", eam::ui::icons::kFolder)) OpenPath(ExeDir() + L"\\addons");
    ImGui::SameLine();
    if (eam::ui::Button("config.json", eam::ui::icons::kFile)) OpenPath(ExeDir() + L"\\addons\\config.json");

    ImGui::Dummy(ImVec2(0, S(14)));
    ImGui::TextDisabled("更改会即时保存。");
}

} // namespace eam
