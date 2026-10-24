#include "update_offer.h"
#include "toast.h"
#include "../../update/update_check.h"
#include "../../update/updater.h"
#include "../gui_scale.h"
#include "imgui.h"
#include <eam/icons.h>
#include <eam/version.h>
#include <eam/widgets.h>
#include <windows.h>
#include <shellapi.h>
#include <string>

namespace eam {
namespace widgets {

namespace {
bool g_requested = false;          // the About tab asked for it
uint64_t g_closeAskedAt = 0;      // when Lossless Scaling was asked to close for the update (0: not asked)
int g_closeAsked = 0;              // how many windows were asked
int64_t g_offeredAt = 0;           // the check the offer last opened for by itself ("Not now" lasts until the next daily check)
constexpr const char* kPopup = "有可用更新###update_offer";

std::wstring LosslessScalingFolder() {   // the manager lives in LosslessScaling.exe: its folder
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    const size_t cut = dir.find_last_of(L"\\/");
    return cut == std::wstring::npos ? dir : dir.substr(0, cut);
}

void Wrapped(const char* text, bool disabled = false) {
    if (disabled) ImGui::TextDisabled("%s", text); else ImGui::TextUnformatted(text);
}
} // namespace

void OpenUpdateOffer() { g_requested = true; }

void UpdateOffer() {
    const update::Status st = update::Current();
    const bool available = st.state == update::State::Available && !st.latest.empty();
    if (available && (g_requested || (g_offeredAt != st.checkedAt && !update::Skipped(st.latest) && !update::ScalingNow()))) {   // (not over a game that is being scaled)
        g_offeredAt = st.checkedAt;
        ImGui::OpenPopup(kPopup);
    }
    g_requested = false;

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0));
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 33.0f);
    const update::Progress pr = update::CurrentProgress();
    const bool forThis = pr.version == st.latest;
    const update::Phase step = forThis ? pr.step : update::Phase::Idle;
    auto close = [] { ImGui::CloseCurrentPopup(); };

    if (step == update::Phase::Downloading || step == update::Phase::Checking) {
        ImGui::Text("正在下载 LS 插件管理器 %s…", st.latest.c_str());
        const float mb = static_cast<float>(pr.done) / 1048576.0f, of = static_cast<float>(pr.total) / 1048576.0f;
        char label[64];
        if (pr.total) snprintf(label, sizeof label, "%.0f / %.0f MB", mb, of); else snprintf(label, sizeof label, "%.0f MB", mb);
        ImGui::ProgressBar(pr.total ? mb / of : -1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 0), label);
        Wrapped("在投入使用前，会先对照 GitHub 上该版本的指纹进行校验。", true);
        ImGui::Dummy(ImVec2(0, S(4)));
        if (ui::Button("取消", ui::icons::kClose)) update::CancelDownload();
    } else if (step == update::Phase::Ready) {
        if (pr.fingerprintChecked) ImGui::Text("LS 插件管理器 %s 已下载并校验完成。", st.latest.c_str());
        else ImGui::Text("LS 插件管理器 %s 已下载。GitHub 未提供它的指纹，因此只校验了文件大小与安装程序的名称和版本。", st.latest.c_str());
        ImGui::Dummy(ImVec2(0, S(2)));
        Wrapped("安装前需要关闭 Lossless Scaling：安装程序会等它退出，完成更新（照常会先备份）并重新启动。"
                "你的设置会全部保留。");
        ImGui::Dummy(ImVec2(0, S(2)));
        Wrapped("“关闭并安装”会让 Lossless Scaling 自行关闭，效果与你点它的窗口 X 相同：进行中的缩放会立即结束。", true);
        ImGui::Dummy(ImVec2(0, S(6)));
        if (ui::Button("关闭并安装", ui::icons::kDownload, ui::ButtonKind::Primary)) {
            std::string error;
            if (!update::StartSetup(LosslessScalingFolder(), error)) ToastShow("无法启动安装程序：" + error, ToastType::Error, 8.0f);
            else { g_closeAsked = update::AskLosslessScalingToClose(); g_closeAskedAt = GetTickCount64(); }
        }
        ImGui::SameLine();
        if (ui::Button("我关闭后再安装", ui::icons::kCheck)) {
            std::string error;
            g_closeAskedAt = 0; g_closeAsked = 0;   // (nothing is asked this time: an earlier "Close it and install" must not show here)
            if (!update::StartSetup(LosslessScalingFolder(), error)) ToastShow("无法启动安装程序：" + error, ToastType::Error, 8.0f);
        }
        ImGui::SameLine();
        if (ui::Button("稍后", ui::icons::kClose)) close();   // it stays downloaded: the About tab's button brings this back
    } else if (step == update::Phase::Handed) {
        ImGui::TextUnformatted("安装程序正在等待 Lossless Scaling 关闭。");
        ImGui::Dummy(ImVec2(0, S(2)));
        const uint64_t waited = g_closeAskedAt ? GetTickCount64() - g_closeAskedAt : 0;
        if (g_closeAskedAt && g_closeAsked > 0 && waited < 10000) {
            Wrapped("已请求 Lossless Scaling 关闭，它应该很快就会退出。随后安装程序会更新它并重新启动。");
        } else if (g_closeAskedAt) {
            Wrapped(g_closeAsked > 0 ? "请求后 Lossless Scaling 没有关闭（可能它的某个对话框还开着，或正忙）。"
                                     : "Lossless Scaling 没有可请求关闭的窗口（多半在系统托盘里）。");
            Wrapped("请自行关闭（它的窗口，或右键任务栏时钟旁的图标并选“退出”），也可以在这里直接结束：进行中的缩放会立即丢失。", true);
            ImGui::Dummy(ImVec2(0, S(4)));
            if (ui::Button("再次请求", ui::icons::kCheck)) { g_closeAsked = update::AskLosslessScalingToClose(); g_closeAskedAt = GetTickCount64(); }
            ImGui::SameLine();
            if (ui::Button("立即结束", ui::icons::kClose)) update::EndLosslessScalingNow();
            ImGui::SameLine();
        } else {
            Wrapped("请关闭 Lossless Scaling（它的窗口，或右键任务栏时钟旁的图标并选“退出”）。随后安装程序会更新它并重新启动。");
        }
        ImGui::Dummy(ImVec2(0, S(6)));
        if (ui::Button("确定", ui::icons::kCheck, ui::ButtonKind::Primary)) close();
    } else {   // the offer (or the offer again after a failed or cancelled try)
        ImGui::Text("有新版 LS 插件管理器 %s。", st.latest.c_str());
        ImGui::TextDisabled("你当前的版本是 %s。", EAM_VERSION_STRING);
        if (forThis && pr.step == update::Phase::Failed && pr.error != "cancelled") {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::V(ui::theme::kWarn));
            ImGui::TextWrapped("下载没有成功：%s。", pr.error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, S(2)));
        if (!st.notes.empty()) {   // the release's own notes (GitHub's), as plain text, in a box of their own
            ImGui::Text("%s 更新内容：", st.latest.c_str());
            ImGui::BeginChild("##notes", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12.0f), ImGuiChildFlags_Borders);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(st.notes.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
        }
        if (ImGui::SmallButton(st.notes.empty() ? "更新内容" : "发布页面")) ShellExecuteA(nullptr, "open", st.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (st.notes.empty()) { ImGui::SameLine(); ImGui::TextDisabled("（发布页面）"); }
        ImGui::Dummy(ImVec2(0, S(6)));
        const bool canDownload = !st.zipUrl.empty() && !st.zipSha256.empty();   // (a download GitHub lists no SHA-256 for is refused by the updater)
        if (!canDownload) ImGui::BeginDisabled();
        if (ui::Button("下载并安装", ui::icons::kDownload, ui::ButtonKind::Primary)) { update::ForgetDownload(); update::StartDownload(st); }
        if (!canDownload) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ui::Button("暂不", ui::icons::kClose)) close();
        ImGui::SameLine();
        if (ui::Button("此版本不再询问")) { update::Skip(st.latest); close(); }
        if (!canDownload) Wrapped(st.zipUrl.empty() ? "该版本未附带下载文件：请前往它的发布页面。" : "GitHub 未给出此下载的校验和，这里无法校验：请前往发布页面。", true);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

} // namespace widgets
} // namespace eam
