#include "eam/widgets.h"
#include "tab_addons.h"
#include "../gui_scale.h"
#include "../../../sdk/include/eam/version.h"
#include "../gui_style.h"
#include "../../addon/addon_manager.h"
#include "../../config/config_manager.h"
#include "../widgets/addon_card.h"
#include "../widgets/toast.h"
#include "../widgets/empty_state.h"
#include "../widgets/runtime_list.h"
#include "../widgets/tooltip.h"
#include "../widgets/toggle_switch.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

namespace eam {

// ---- installing an addon ------------------------------------------------------------------------------------------
static std::wstring s_installSource;        // what the user picked, waiting for confirmation
static bool s_askInstall = false;
static std::string s_removeId;              // the addon waiting for the remove confirmation
static bool s_askRemove = false;

void RequestInstallFromPath(const std::wstring& path) { s_installSource = path; s_askInstall = true; }

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s((size_t)WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], (int)s.size(), nullptr, nullptr);
    return s;
}

// The system's own open dialog, for a folder or for a .zip / .dll. Only ever shown when the user clicked for it.
static bool PickPath(bool folder, std::wstring& out) {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0; dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
        if (!folder) { COMDLG_FILTERSPEC f[] = { { L"插件（.zip 或 .dll）", L"*.zip;*.dll" }, { L"所有文件", L"*.*" } }; dlg->SetFileTypes(2, f); }
        dlg->SetTitle(folder ? L"选择插件文件夹" : L"选择插件 zip 或 DLL");
        if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); ok = true; }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}

static char s_searchBuffer[256] = {};


// Selection is by addon id so it survives reordering, and is remembered across sessions.
static std::string s_selectedId;
static bool s_selectionLoaded = false;
static std::string s_tabsOpenedFor; // addon the detail tab bar last picked a default tab for

// Inline config editor state (one file at a time, loaded when the Config tab is shown).
static std::wstring s_cfgPath;
static std::vector<char> s_cfgBuf;
static bool s_cfgDirty = false;
static constexpr size_t kCfgBufSize = 1024 * 1024;

static bool s_cfgTooBig = false;   // file does not fit the edit buffer: editing it would truncate it

static void LoadConfigFile(const std::wstring& path) {
    s_cfgPath = path;
    s_cfgBuf.assign(kCfgBufSize, 0);
    s_cfgDirty = false;
    s_cfgTooBig = false;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return;
    std::stringstream buf;
    buf << file.rdbuf();
    const std::string content = buf.str();
    if (content.size() >= kCfgBufSize || content.find('\0') != std::string::npos) { s_cfgTooBig = true; return; }   // (too big, or not plain text: a NUL, as in a UTF-16 file, would cut it short on Save)
    memcpy(s_cfgBuf.data(), content.data(), content.size());
}

static void SaveConfigFile() {
    if (s_cfgPath.empty() || s_cfgTooBig) return;
    // Write a sibling file and swap it in so an interrupted save cannot leave half a config.
    const std::wstring tmp = s_cfgPath + L".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file << s_cfgBuf.data();
        file.flush();
        if (!file) {
            widgets::ToastShow("无法写入配置文件", widgets::ToastType::Error);
            return;
        }
    }
    if (!MoveFileExW(tmp.c_str(), s_cfgPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        widgets::ToastShow("无法替换配置文件", widgets::ToastType::Error);
        return;
    }
    s_cfgDirty = false;
    widgets::ToastShow("配置已保存", widgets::ToastType::Success);
}

// Lower case for the search box. Not ::tolower on plain chars: that is undefined for non-ASCII text (an addon named with an accented letter).
static std::string LowerCase(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The search box: every word typed must appear somewhere in the addon's name, author, id, description or tags (any case), so
// "fsr upscaler" or "nvidia dlss" find what people expect. Words are separated by spaces.
static bool MatchesFilter(const AddonInfo& addon, const char* filter) {
    if (!filter) return true;
    std::string haystack = addon.GetDisplayName() + '\n' + addon.GetDisplayAuthor() + '\n' + addon.id + '\n' + addon.manifest.description;
    for (const auto& tag : addon.manifest.tags) haystack += '\n' + tag;
    haystack = LowerCase(haystack);
    const std::string words = LowerCase(filter);
    for (size_t at = 0; at < words.size();) {
        const size_t from = words.find_first_not_of(' ', at);
        if (from == std::string::npos) break;
        const size_t to = (std::min)(words.find(' ', from), words.size());
        if (haystack.find(words.substr(from, to - from)) == std::string::npos) return false;
        at = to;
    }
    return true;
}

static void SelectAddon(const AddonInfo& addon) {
    if (s_selectedId == addon.id) return;
    s_selectedId = addon.id;
    auto& cfg = ConfigManager::Instance();
    cfg.GlobalSet("ui", "selected_addon", addon.id);
    cfg.Save();
}

// The card and the detail pane both flip an addon through here, so they behave identically.
static void ApplyToggle(AddonManager* manager, int index, bool enable) {
    const std::vector<std::string> off = manager->ToggleAddon(index, enable);
    std::string also;   // the addons it cannot run beside, turned off
    for (const std::string& name : off) also += (also.empty() ? "" : ", ") + name;
    if (manager->GetAddons()[index].RequiresRestart()) {
        widgets::ToastShow(enable ? "重启 Lossless Scaling 以启用此插件"
                                  : "重启 Lossless Scaling 以停用此插件",
                           widgets::ToastType::Warning, 5.0f);
    } else if (enable && !manager->GetAddons()[index].IsLoaded()) {
        const std::string& why = manager->GetAddons()[index].errorMessage;
        widgets::ToastShow(why.empty() ? "插件已启用，但未能加载" : why,
                           widgets::ToastType::Error, 6.0f);
    } else if (!also.empty()) {
        widgets::ToastShow("插件已启用。已关闭 " + also + "：它们无法同时运行。", widgets::ToastType::Success, 5.0f);
    } else {
        widgets::ToastShow(enable ? "插件已启用" : "插件已停用",
                           enable ? widgets::ToastType::Success : widgets::ToastType::Info);
    }
}

static void StatusText(const char* text, ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

static void RenderSettingsPane(AddonManager* manager, const AddonInfo& addon, int index) {
    ImGui::Dummy(ImVec2(0, S(4)));
    if (!addon.enabled) {
        ImGui::TextDisabled("此插件已停用。请先开启（右上角）才能配置。");
        return;
    }
    if (!addon.IsLoaded()) {
        if (!addon.errorMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.816f, 0.502f, 0.502f, 1));
            ImGui::TextWrapped("%s", addon.errorMessage.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, S(4)));
        }
        if (addon.RequiresRestart()) {
            ImGui::TextWrapped("重启 Lossless Scaling 以加载此插件。");
        } else {
            ImGui::TextWrapped("此插件已启用但未加载。");
            if (eam::ui::Button("立即加载", eam::ui::icons::kPlay, eam::ui::ButtonKind::Primary)) {
                manager->LoadAddonNow(index);
                widgets::ToastShow(manager->GetAddons()[index].IsLoaded() ? "插件已加载" : "无法加载插件",
                                   manager->GetAddons()[index].IsLoaded() ? widgets::ToastType::Success
                                                                          : widgets::ToastType::Error);
            }
        }
        return;
    }
    // The addon draws its own controls into this child; long status lines can scroll sideways.
    ImGui::BeginChild("##addon_settings", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    const float y0 = ImGui::GetCursorPosY();
    manager->RenderAddonSettings(index);
    if (ImGui::GetCursorPosY() == y0)
        ImGui::TextWrapped("此插件未提供任何控件。部分插件仅在对游戏进行缩放时才显示设置；"
                           "若始终没有显示，请到日志页查看错误。");
    ImGui::EndChild();
}

static void RenderOverviewPane(AddonManager* manager, const AddonInfo& addon) {
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::BeginChild("##addon_overview", ImVec2(0, 0), false);

    if (!addon.manifest.description.empty())
        ImGui::TextWrapped("%s", addon.manifest.description.c_str());
    else
        ImGui::TextDisabled("暂无描述。");

    if (!addon.manifest.tags.empty()) {
        ImGui::Dummy(ImVec2(0, S(8)));
        for (const auto& tag : addon.manifest.tags) {
            ImGui::PushStyleColor(ImGuiCol_Button, eam::ui::theme::V(eam::ui::theme::kSelection));
            ImGui::SmallButton(tag.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }

    if (!addon.manifest.dependencies.empty()) {
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::TextDisabled("依赖");
        for (const auto& dep : addon.manifest.dependencies) {
            const AddonInfo* found = nullptr;
            for (const auto& other : manager->GetAddons())
                if (other.id == dep) { found = &other; break; }
            ImGui::BulletText("%s", dep.c_str());
            ImGui::SameLine();
            if (!found)                    StatusText("（未安装）", ImVec4(0.816f, 0.502f, 0.502f, 1));
            else if (!found->enabled)      StatusText("（已停用）", eam::ui::theme::V(eam::ui::theme::kWarn));
            else                           ImGui::TextDisabled("（已启用）");
        }
    }

    if (!addon.errorMessage.empty()) {
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.816f, 0.502f, 0.502f, 1));
        ImGui::TextWrapped("错误：%s", addon.errorMessage.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

static void RenderConfigPane(const AddonInfo& addon) {
    ImGui::Dummy(ImVec2(0, S(4)));
    if (s_cfgPath != addon.configPath) LoadConfigFile(addon.configPath);

    if (s_cfgTooBig) {
        ImGui::TextWrapped("此文件超出编辑器可安全容纳的大小，请改用文本编辑器打开。");
        return;
    }
    if (eam::ui::Button("保存", eam::ui::icons::kSave, eam::ui::ButtonKind::Primary)) SaveConfigFile();
    widgets::Tip("将下方文本写入插件的配置文件。多数插件仅在加载时读取，因此可能需要重启。");
    ImGui::SameLine();
    if (eam::ui::Button("重新加载", eam::ui::icons::kReset)) LoadConfigFile(addon.configPath);
    widgets::Tip("放弃当前修改并重新读取文件。");
    if (s_cfgDirty) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn));
        ImGui::TextUnformatted("未保存的更改");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, S(2)));

    ImFont* mono = MonoFont();
    if (mono) ImGui::PushFont(mono, 0.0f);
    if (ImGui::InputTextMultiline("##configedit", s_cfgBuf.data(), s_cfgBuf.size(),
                                  ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_AllowTabInput))
        s_cfgDirty = true;
    if (mono) ImGui::PopFont();
}

static void RenderDetail(AddonManager* manager, AddonInfo& addon, int index) {
    // Header: icon, name (larger), version and author, then the addon's own description; the switch on the right.
    const float iconSize = S(44.0f);
    widgets::DrawAddonIcon(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), iconSize, addon, addon.enabled);
    ImGui::Dummy(ImVec2(iconSize, iconSize));
    ImGui::SameLine(0, S(12));
    ImGui::BeginGroup();
    if (ImFont* title = TitleFont()) ImGui::PushFont(title, ImGui::GetStyle().FontSizeBase * 1.2f);
    ImGui::TextUnformatted(addon.GetDisplayName().c_str());
    if (TitleFont()) ImGui::PopFont();
    ImGui::TextDisabled("v%s  \xc2\xb7  %s", addon.GetDisplayVersion().c_str(), addon.GetDisplayAuthor().c_str());
    ImGui::EndGroup();
    if (addon.OlderBuild(EAM_VERSION_STRING)) {   // a test build or an old file left in the folder: Setup replaces it
        ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn));
        ImGui::TextWrapped("此插件的文件版本为 %s，低于管理器版本（%s）：可能是测试构建或残留的旧文件。请关闭 Lossless Scaling，运行安装程序（重新安装）以恢复正式版文件。", addon.dllVersion.c_str(), EAM_VERSION_STRING);
        ImGui::PopStyleColor();
    }

    const float switchWidth = ImGui::GetFrameHeight() * 0.8f * 1.8f;
    ImGui::SameLine(ImGui::GetWindowWidth() - switchWidth - ImGui::GetStyle().WindowPadding.x - S(4));
    bool enabled = addon.enabled;
    if (addon.manifest.wip) ImGui::BeginDisabled();
    if (widgets::ToggleSwitch("##detail_enable", &enabled))
        ApplyToggle(manager, index, enabled);
    if (addon.manifest.wip) ImGui::EndDisabled();
    widgets::Tip(addon.manifest.wip ? "开发中：此插件暂无法开启。"
                                    : "开启或关闭此插件。除非插件声明需要重启 Lossless Scaling，否则会立即加载或卸载。");

    // Status line.
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::TextDisabled("状态");
    ImGui::SameLine();
    if (addon.faulted)            StatusText("故障", ImVec4(0.816f, 0.502f, 0.502f, 1));
    else if (addon.IsLoaded())    StatusText("已加载", eam::ui::theme::V(eam::ui::theme::kAccent));
    else                          ImGui::TextDisabled("未加载");

    ImGui::SameLine(0, S(24));
    ImGui::TextDisabled("安全性");
    ImGui::SameLine();
    switch (addon.security) {
        case SecurityVerdict::Trusted:  StatusText("可信", eam::ui::theme::V(eam::ui::theme::kAccent)); break;
        case SecurityVerdict::Tampered: StatusText("已被篡改！", ImVec4(0.816f, 0.502f, 0.502f, 1)); break;
        default:                        ImGui::TextDisabled("未知"); break;
    }

    {   // remove, at the right end of the status line
        const float rw = ImGui::CalcTextSize("移除").x + ImGui::GetFontSize() * 3.2f;
        ImGui::SameLine(ImGui::GetWindowWidth() - rw - ImGui::GetStyle().WindowPadding.x - S(4));
        if (eam::ui::Button("移除", eam::ui::icons::kTrash, eam::ui::ButtonKind::Danger)) { s_removeId = addon.id; s_askRemove = true; }
        widgets::Tip("将此插件从 Lossless Scaling 中移除。操作前会先确认。其文件夹会被移到 addons/.removed，不会被删除。");
    }

    if (!addon.errorMessage.empty() && addon.IsLoaded()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.816f, 0.502f, 0.502f, 1));
        ImGui::TextWrapped("%s", addon.errorMessage.c_str());
        ImGui::PopStyleColor();
    }
    if (addon.RequiresRestart()) {
        ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn));
        ImGui::TextWrapped("此插件需要重启 Lossless Scaling 才能应用启用/停用更改。");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, S(4)));

    // Tabs. The first time an addon is shown, land on its settings if it has any.
    // Capabilities are only known once the DLL is loaded, so an unloaded addon always gets the
    // Settings tab: that is where "Load now" and the reason it is not loaded are shown.
    const bool hasSettings = !addon.IsLoaded() || (addon.capabilities & EAM_CAP_HAS_SETTINGS) != 0;
    const bool hasConfig = !addon.configPath.empty();
    const bool firstShow = (s_tabsOpenedFor != addon.id);
    s_tabsOpenedFor = addon.id;

    if (ImGui::BeginTabBar("##detail_tabs")) {
        if (hasSettings && ImGui::BeginTabItem("设置", nullptr,
                                               firstShow ? ImGuiTabItemFlags_SetSelected : 0)) {
            RenderSettingsPane(manager, addon, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("概览", nullptr,
                                (firstShow && !hasSettings) ? ImGuiTabItemFlags_SetSelected : 0)) {
            RenderOverviewPane(manager, addon);
            ImGui::EndTabItem();
        }
        if (hasConfig && ImGui::BeginTabItem("配置")) {
            RenderConfigPane(addon);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void RenderTabAddons(AddonManager* manager) {
    if (!manager) return;
    auto& addons = manager->GetAddons();

    if (!s_selectionLoaded) {
        s_selectionLoaded = true;
        s_selectedId = ConfigManager::Instance().GlobalGetOr<std::string>("ui", "selected_addon", "");
    }

    auto resolveSelection = [&]() {
        for (int i = 0; i < (int)addons.size(); i++)
            if (addons[i].id == s_selectedId) return i;
        if (!addons.empty()) { SelectAddon(addons[0]); return 0; }   // the remembered one is gone (removed, renamed): pick the first, and remember that
        return -1;
    };
    int sel = resolveSelection();

    // Sidebar (left, slim): the search box, the install buttons and the addons, one row each; the detail pane takes the rest.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float listWidth = std::clamp(avail * 0.26f, S(220.0f), S(280.0f));
    ImGui::BeginChild("AddonList", ImVec2(listWidth, -1), false);

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();   // Ctrl+F: jump to the search box
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##addon_search", "搜索（Ctrl+F）", s_searchBuffer, sizeof(s_searchBuffer));
    widgets::Tip("按名称、作者或标签筛选列表。");
    ImGui::Dummy(ImVec2(0, S(2)));

    if (eam::ui::Button("安装插件", eam::ui::icons::kDownload, eam::ui::ButtonKind::Primary)) ImGui::OpenPopup("##install_menu");
    widgets::Tip("从文件夹、.zip 或 .dll 添加插件。它会被复制到插件文件夹并以停用状态安装。"
                 "也可以直接将其拖放到本窗口。");
    if (ImGui::BeginPopup("##install_menu")) {
        std::wstring picked;
        if (ImGui::Selectable("从文件夹…") && PickPath(true, picked)) { s_installSource = picked; s_askInstall = true; }
        if (ImGui::Selectable("从 zip 或 DLL…") && PickPath(false, picked)) { s_installSource = picked; s_askInstall = true; }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (eam::ui::IconButton("##open_folder", eam::ui::icons::kFolder, ImGui::GetFrameHeight())) ShellExecuteW(nullptr, L"open", manager->GetAddonsPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    widgets::Tip("打开插件所在的文件夹，可手动移除或更新插件。");

    if (s_askInstall) { ImGui::OpenPopup("安装插件"); s_askInstall = false; }
    if (ImGui::BeginPopupModal("安装插件", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextWrapped("安装此插件？");
        ImGui::TextDisabled("%s", WideToUtf8(s_installSource).c_str());
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::TextWrapped("插件是在 Lossless Scaling 内部运行的程序，拥有与 Lossless Scaling 本身相同的权限。"
                           "请只安装来自可信来源的插件。插件安装后默认停用，可通过其开关开启。");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(6)));
        if (eam::ui::Button("安装", eam::ui::icons::kDownload, eam::ui::ButtonKind::Primary)) {
            const AddonManager::InstallResult r = manager->InstallAddon(s_installSource);
            widgets::ToastShow(r.message, r.ok ? widgets::ToastType::Success : widgets::ToastType::Error, 7.0f);
            if (r.ok) s_selectedId = r.id;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (eam::ui::Button("取消", eam::ui::icons::kClose)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (s_askRemove) { ImGui::OpenPopup("移除插件"); s_askRemove = false; }
    if (ImGui::BeginPopupModal("移除插件", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        int ri = -1;
        for (int i = 0; i < (int)addons.size(); ++i) if (addons[i].id == s_removeId) ri = i;
        if (ri < 0) { ImGui::TextUnformatted("该插件已不存在。"); if (eam::ui::Button("关闭")) ImGui::CloseCurrentPopup(); }
        else {
            const std::string nm = addons[ri].GetDisplayName();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextWrapped("移除 %s？", nm.c_str());
            ImGui::Dummy(ImVec2(0, S(4)));
            ImGui::TextWrapped("它将被停用，其文件夹会移动到 addons/.removed（不会删除任何内容；把文件夹移回即可恢复）。"
                               "它的设置会保留，重新安装后即可沿用。");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, S(6)));
            if (eam::ui::Button("移除", eam::ui::icons::kTrash, eam::ui::ButtonKind::Danger)) {
                const AddonManager::InstallResult rr = manager->RemoveAddon(ri);
                widgets::ToastShow(rr.message, rr.ok ? widgets::ToastType::Success : widgets::ToastType::Error, 7.0f);
                if (rr.ok) s_selectedId.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (eam::ui::Button("取消", eam::ui::icons::kClose)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::PushStyleColor(ImGuiCol_Separator, eam::ui::theme::V(eam::ui::theme::kBorder));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
    if (addons.empty()) {
        widgets::EmptyState(listWidth, "尚未安装任何插件", "使用上方的“安装插件”，或将文件夹、.zip 或 .dll 拖放到本窗口。");
    }
    for (int i = 0; i < (int)addons.size(); i++) {
        if (!MatchesFilter(addons[i], s_searchBuffer)) continue;
        bool toggled = false;
        if (widgets::AddonCard(addons[i], i, i == sel, &toggled))
            SelectAddon(addons[i]);
        if (toggled) ApplyToggle(manager, i, addons[i].enabled);
        ImGui::Dummy(ImVec2(0, S(1)));
    }
    // the runtime files the addons use, at the bottom of the list (right under the addons when they fill it)
    {
        const std::wstring lsDir = std::filesystem::path(manager->GetAddonsPath()).parent_path().wstring();
        const std::vector<RuntimeFile> runtimes = RuntimeFiles(addons, lsDir);
        if (!runtimes.empty()) widgets::RuntimeListAtBottom(runtimes);
    }
    ImGui::EndChild();

    sel = resolveSelection();
    ImGui::SameLine();
    ImGui::BeginChild("AddonDetail", ImVec2(0, -1), true);
    if (sel >= 0)
        RenderDetail(manager, addons[sel], sel);
    else
        ImGui::TextDisabled("请选择一个插件。");
    ImGui::EndChild();
}

} // namespace eam
