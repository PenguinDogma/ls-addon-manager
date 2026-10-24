// The addon's settings panel, drawn by the manager in its window (the window's thread). It works on a copy of the settings and hands the copy
// back (Commit) when something changed. The sections start closed; the person opens what they need.
#include "engine/sr_engine.h"
#include "addon/state.h"
#include "addon/present_hook.h"
#include "addon/screenshot.h"
#include "addon/hud_editor.h"
#include "imgui.h"
#include <eam/widgets.h>
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace nr {

// Wrapped tooltip for the widget just submitted, after a short hover delay.
static void Tip(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    const char* hint = eam::ui::SliderHint();   // sliders add "default, double-click resets, Ctrl+scroll fine-tunes"
    ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f); ImGui::TextUnformatted(text);
    if (hint) { ImGui::Dummy(ImVec2(0, 2)); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]); ImGui::TextUnformatted(hint); ImGui::PopStyleColor(); }
    ImGui::PopTextWrapPos(); ImGui::EndTooltip();
}
// Wrapped help text in the disabled colour (ImGui::TextDisabled does not wrap, so long lines ran off the panel).
static void Note(const char* fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", b);
    ImGui::PopStyleColor();
}
// Every collapsible section of the panel starts closed (eam::ui::SectionHeader with no second argument); the person opens what they need.
// A titled block of the panel: some room, a thin line, the title in the small capitals of the other apps, and a little room under it.
static void Block(const char* title, bool first = false) {
    const float u = ImGui::GetFontSize();
    if (!first) { ImGui::Dummy(ImVec2(0, u * 0.7f)); ImGui::Separator(); ImGui::Dummy(ImVec2(0, u * 0.5f)); }
    eam::ui::SectionLabel(title);
    ImGui::Dummy(ImVec2(0, u * 0.35f));
}

void DrawPanel() {
    FollowModelChoice();   // before the panel copies the settings: a model file chosen in the Runtimes list is not saved over
    Config c; { std::lock_guard<std::mutex> lk(g_settingsMutex); c = g_config; }
    bool changed = false, createChanged = false, tapChanged = false;
    // Every slider bound to a field of the params gets that field's default (a fresh NrParams): a tick on the groove, a ring on the
    // knob while it differs, double-click to reset. Sliders bound to anything else simply have no default.
    static const NrParams kDefaults;
    auto SL = [&](const char* label, float* v, float lo, float hi, const char* fmt = "%.2f") {
        const char* base = (const char*)&c.p; const char* pv = (const char*)v; const float* def = nullptr;
        if (pv >= base && pv < base + sizeof(NrParams)) def = (const float*)((const char*)&kDefaults + (pv - base));
        return eam::ui::SliderFloat(label, v, lo, hi, fmt, 0, def);
    };

    // status
    std::string status = Status(), frameInfo, adapterName; bool hasDisplay;
    { std::lock_guard<std::mutex> lk(g_textMutex); frameInfo = g_frameText.empty() ? g_frameText : g_frameText + " (" + g_encodingText + ")"; adapterName = g_cardName; hasDisplay = g_cardDrivesDisplay; }
    Block("状态", true);
    if (g_off) {
        std::string why; { std::lock_guard<std::mutex> lock(g_textMutex); why = g_offReason; }
        ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kDanger)); ImGui::Text("已禁用：%s", why.c_str()); ImGui::PopStyleColor();
        ImGui::SameLine(); if (ImGui::SmallButton("重新启用")) SwitchOn();
        Tip("它自动关闭后，用它重新打开。如果再次自动关闭，说明上面的原因依然存在。");
    }
    else { ImGui::PushStyleColor(ImGuiCol_Text, g_runs ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::Text("%s", status.c_str()); ImGui::PopStyleColor(); }
    if (g_engine.IsFailed()) { ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kDanger), "引擎：%s", g_engine.Stats().lastError); ImGui::SameLine(); if (ImGui::SmallButton("重试引擎")) RestartEngine(); Tip("尝试在当前显卡上重新启动 DLSS 模型。"); }
    if (!g_off) {   // ---- what stands out in the last few seconds (diagnosis.h), in plain words
        std::vector<nr::diag::Finding> findings; { std::lock_guard<std::mutex> lk(g_textMutex); findings = g_findings; }
        if (!findings.empty()) {
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.25f));
            ImGui::TextDisabled("值得关注");
            Tip("根据本次会话最近几秒的情况得出：游戏帧的均匀程度、模型在等待什么、自动画质做了什么。大约每五秒更新一次。");
            for (const auto& f : findings) {
                const ImVec4 col = f.level == nr::diag::Level::Problem ? eam::ui::theme::V(eam::ui::theme::kDanger) : (f.level == nr::diag::Level::Note ? eam::ui::theme::V(eam::ui::theme::kWarn) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::PushStyleColor(ImGuiCol_Text, col); ImGui::TextWrapped("%s", f.what.c_str()); ImGui::PopStyleColor();
                ImGui::Indent(ImGui::GetFontSize() * 1.0f); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); ImGui::TextWrapped("%s", f.tryThis.c_str()); ImGui::PopStyleColor(); ImGui::Unindent(ImGui::GetFontSize() * 1.0f);
            }
        }
    }
    if (!kScalerAddon) {   // ---- Requirements: what this needs, what was found, and what to do about anything missing
        const bool have = RequirementsScanned();
        const req::Report rep = Requirements({ g_engine.IsFailed(), g_engine.IsReady(), g_runs > 0, g_engine.Stats().lastError });
        Block("需求");
        ImGui::TextWrapped("模型文件需要自行提供：nvngx_dlssnr.dll 不随本插件附带，也永远不会被自动下载。");
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.25f));
        Note("1. 把你的 nvngx_dlssnr.dll 放进 Lossless Scaling 文件夹，与 LosslessScaling.exe 同目录。下方的浏览按钮会帮你复制过去。");
        Note("2. 按下测试兼容性，检查它在你的显卡上能否正常工作。");
        Note("3. 打开下方的启用开关，然后启动游戏并照常缩放。");
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f));
        // the verdict, in one line
        if (!have) ImGui::TextDisabled("正在检查...");
        else if (rep.overall == req::Level::Missing) { ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kDanger)); ImGui::TextWrapped("尚未就绪。%s", rep.headline.c_str()); ImGui::PopStyleColor(); }
        else if (rep.overall == req::Level::Note) { ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("已就绪，但有一点需要注意。%s", rep.headline.c_str()); ImGui::PopStyleColor(); }
        else { ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kAccent), "一切就绪。"); }
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
        {   // (always open)
            for (const auto& row : rep.rows) {
                const ImVec4 col = row.level == req::Level::Ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : (row.level == req::Level::Note ? eam::ui::theme::V(eam::ui::theme::kWarn) : eam::ui::theme::V(eam::ui::theme::kDanger));
                ImGui::TextColored(col, row.level == req::Level::Ok ? "正常   " : (row.level == req::Level::Note ? "注意   " : "缺失   "));
                ImGui::SameLine(ImGui::GetFontSize() * 5.2f); ImGui::Text("%s", row.label.c_str()); ImGui::SameLine(ImGui::GetFontSize() * 13.0f); ImGui::TextWrapped("%s", row.value.c_str());
                if (!row.hint.empty()) { ImGui::Indent(ImGui::GetFontSize() * 1.7f); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); ImGui::TextWrapped("%s", row.hint.c_str()); ImGui::PopStyleColor(); ImGui::Unindent(ImGui::GetFontSize() * 1.7f); }
            }
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f));
            const bool placing = Browsing();
            if (placing) ImGui::BeginDisabled();
            if (ImGui::SmallButton(placing ? "正在等待文件对话框..." : "浏览模型文件...")) BrowseForModel();
            if (placing) ImGui::EndDisabled();
            Tip("选择你自己的 nvngx_dlssnr.dll：它会被复制到 Lossless Scaling 文件夹。已存在的同名文件会移到备份文件夹，绝不会删除。不会下载任何东西。");
            ImGui::SameLine();
            const bool testing = SelfTesting();
            if (testing) ImGui::BeginDisabled();
            if (ImGui::SmallButton(testing ? "正在测试..." : "测试兼容性")) RunSelfTest();
            if (testing) ImGui::EndDisabled();
            Tip("在独立程序中、于你的显卡上运行一次模型文件，并告知它能否正常工作。会占用显卡几秒钟，请在启动游戏前进行。即使模型表现异常，也不会影响 Lossless Scaling 运行。");
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.15f));
            if (ImGui::SmallButton("打开 Lossless Scaling 文件夹")) ShellExecuteW(nullptr, L"open", g_lsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            Tip("模型文件 nvngx_dlssnr.dll 的存放位置：LosslessScaling.exe 旁边。");
            ImGui::SameLine();
            if (ImGui::SmallButton("重新检查")) ScanRequirements();
            Tip("重新检查显卡、NVIDIA 驱动、模型文件和辅助 DLL。放入文件后使用。");
            const std::wstring reportFile = g_addonDir + L"\\compatibility_report.txt";
            if (GetFileAttributesW(reportFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ImGui::SameLine();
                if (ImGui::SmallButton("打开兼容性报告")) ShellExecuteW(nullptr, L"open", reportFile.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                Tip("关于上次兼容性测试的简短文本：包含你的显卡、驱动、Windows、模型文件的名称、版本和大小，以及结果。可粘贴到 issue 或兼容性表格中。其中不含文件夹或用户名；模型文件只显示名称、版本、大小和哈希前 16 位（用于区分同名的不同构建），失败时还会附上 NVIDIA 代码返回的信息。");
                ImGui::SameLine();
                if (ImGui::SmallButton("复制报告")) {
                    FILE* rf = nullptr; std::string text;
                    if (_wfopen_s(&rf, reportFile.c_str(), L"rb") == 0 && rf) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, rf)) > 0) text.append(buf, n); fclose(rf); }
                    if (!text.empty()) ImGui::SetClipboardText(text.c_str());
                }
                Tip("将兼容性报告复制到剪贴板，可直接粘贴到 issue 中。");
            }
            { bool ok; const std::string msg = BrowseResult(ok);
              if (!msg.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("%s", msg.c_str()); ImGui::PopStyleColor(); } }
        }
    }
    else if (kXessScaler) {   // ---- XeSS's requirements: a DirectX 12 card with Shader Model 6.4, and Intel's runtime, which ships in the addon's xess folder
        Block("需求");
        const std::wstring runtime = g_addonDir + L"\\xess\\libxess.dll";
        const bool present = GetFileAttributesW(runtime.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (present) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kAccent), "Intel 的 XeSS 运行时已就位（随本插件附带）。");
        else ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kDanger), "缺少 Intel 的 XeSS 运行时：插件的 xess 文件夹中应有 libxess.dll。请重新安装插件。");
        Note("适用于任何支持 DirectX 12 和 Shader Model 6.4 的显卡（Intel Arc、NVIDIA 或 AMD）；在 Intel Arc 上会使用显卡的 XMX 单元运行。在 Lossless Scaling 中"
             "选择 NIS 作为缩放类型（XeSS 接替该处理趟），并让游戏以小于屏幕的窗口运行，例如 4K 屏幕上用 2560x1440；"
             "若窗口为屏幕原始大小，则会改为抗锯齿。帧生成可开可关。多个上采样器同时只能运行一个。");
    }
    else if (kFsrScaler) {   // ---- FSR's requirements: any DirectX 12 card, and AMD's runtime, which ships in the addon's fsr folder
        Block("需求");
        const std::wstring runtime = g_addonDir + L"\\fsr\\amd_fidelityfx_dx12.dll";
        const bool present = GetFileAttributesW(runtime.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (present) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kAccent), "AMD 的 FSR 运行时已就位（随本插件附带）。");
        else ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kDanger), "缺少 AMD 的 FSR 运行时：插件的 fsr 文件夹中应有 amd_fidelityfx_dx12.dll。请重新安装插件。");
        Note("适用于任何支持 DirectX 12 的显卡（AMD、NVIDIA 或 Intel）。在 Lossless Scaling 中选择 NIS 作为缩放类型（FSR 接替该处理趟），"
             "并让游戏以小于屏幕的窗口运行，例如 4K 屏幕上用 2560x1440；若窗口为屏幕原始大小，则会改为抗锯齿。"
             "帧生成可开可关。多个上采样器（DLSS、FSR、XeSS）同时只能运行一个。");
    }
    else {   // ---- DLAA's requirements: an NVIDIA RTX card, and NVIDIA's runtime, which ships in the addon's dlss folder
        Block("需求");
        const std::wstring runtime = g_addonDir + L"\\dlss\\nvngx_dlss.dll";
        const bool present = GetFileAttributesW(runtime.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (present) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kAccent), "NVIDIA 的 DLSS 运行时已就位（随本插件附带）。");
        else ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kDanger), "缺少 NVIDIA 的 DLSS 运行时：插件的 dlss 文件夹中应有 nvngx_dlss.dll。请重新安装插件。");
        Note("需要 NVIDIA RTX 显卡。在 Lossless Scaling 中选择 NIS 作为缩放类型（DLSS 接替该处理趟），并让游戏以小于屏幕的窗口运行，"
             "这样才有画面可供上采样，例如 4K 屏幕上用 2560x1440。帧生成可开可关：开启时会使用其运动估计。");
    }
    if (!kScalerAddon) {   // looks are Neural Rendering's
    Block("保存的方案（加载并保存你的设置）");
    Note("方案是下方滑块的一组存档。从列表中选一个即可加载。保存会更新当前选中的方案；另存为新方案则以你指定的名称保存当前滑块设置。");
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
    // ---- Saved looks. Pick one to apply it; Save updates it (or asks for a name); Save as new keeps the current sliders under a
    // ---- new name; Delete asks first. A look is the sliders below (model knobs, picture, HUD areas), not the hotkeys or advanced settings.
    {
        static int s_active = -1; static char s_name[48] = ""; static bool s_askSave = false, s_askDelete = false;
        std::vector<std::string> names, datas;
        { std::lock_guard<std::mutex> lk(g_settingsMutex); for (auto& pr : g_looks) { names.push_back(pr.name); datas.push_back(pr.data); } }
        const std::string now = LookToText(c.p);
        if (s_active >= (int)names.size()) s_active = -1;
        // a look saved by an older version lacks newer keys; compare it as it would apply now, so it still reads as unchanged
        for (auto& d : datas) { NrParams t = c.p; ApplyLook(d, t); d = LookToText(t); }
        if (s_active < 0) for (size_t i = 0; i < datas.size(); ++i) if (datas[i] == now) { s_active = (int)i; break; }   // recognise the look already in use
        const bool modified = s_active >= 0 && datas[s_active] != now;
        const std::string label = s_active >= 0 ? names[s_active] + (modified ? "  （已修改）" : "") : std::string("自定义");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13.0f);
        if (ImGui::BeginCombo("保存的方案", label.c_str())) {
            for (int i = 0; i < (int)names.size(); ++i)
                if (ImGui::Selectable(names[i].c_str(), i == s_active)) {
                    const float was = c.p.workingScale;
                    if (ApplyLook(datas[i], c.p)) { changed = true; if (c.p.workingScale != was) createChanged = true; }
                    s_active = i;
                }
            if (names.empty()) ImGui::TextDisabled("还没有保存的方案：先调整滑块，再点击另存为新方案。");
            ImGui::EndCombo();
        }
        Tip("你保存的方案：选一个即可应用。游戏内可用下一个预设快捷键在各方案间循环切换。若方案的模型分辨率不同，模型会在后台重新构建，耗时不到一秒。");
        ImGui::SameLine();
        if (eam::ui::Button("保存", eam::ui::icons::kSave, eam::ui::ButtonKind::Primary)) {
            if (s_active >= 0) {
                std::lock_guard<std::mutex> lk(g_settingsMutex);
                if (s_active < (int)g_looks.size()) g_looks[s_active].data = now;
                changed = true;
            } else { s_name[0] = 0; s_askSave = true; }
        }
        Tip(s_active >= 0 ? "用当前滑块设置更新选中的方案。" : "把当前滑块设置存为新方案：需要输入名称。");
        ImGui::SameLine();
        if (eam::ui::Button("另存为新方案", eam::ui::icons::kPlus)) { s_name[0] = 0; s_askSave = true; }
        Tip("以新名称保存当前滑块设置。若名称已存在，会替换该方案。");
        ImGui::SameLine();   // always shown, so it can be found: greyed out until a look is picked
        if (s_active < 0) ImGui::BeginDisabled();
        if (eam::ui::Button("删除", eam::ui::icons::kTrash, eam::ui::ButtonKind::Danger)) s_askDelete = true;
        if (s_active < 0) ImGui::EndDisabled();
        Tip(s_active >= 0 ? "删除选中的方案。当前滑块设置不会改变。" : "先在列表中选择一个方案，然后点击此项删除它。");
        if (s_askSave) { ImGui::OpenPopup("保存方案"); s_askSave = false; }
        if (ImGui::BeginPopupModal("保存方案", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("方案名称");
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##lookname", s_name, sizeof s_name, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Dummy(ImVec2(0, 4));
            const std::string n = CleanName(s_name);
            const bool can = !n.empty();
            if (!can) ImGui::BeginDisabled();
            if (eam::ui::Button("保存", eam::ui::icons::kCheck, eam::ui::ButtonKind::Primary) || (enter && can)) {
                { std::lock_guard<std::mutex> lk(g_settingsMutex); bool found = false; for (auto& pr : g_looks) if (pr.name == n) { pr.data = now; found = true; } if (!found) g_looks.push_back({ n, now }); }
                s_active = -1; changed = true; ImGui::CloseCurrentPopup();
            }
            if (!can) ImGui::EndDisabled();
            ImGui::SameLine();
            if (eam::ui::Button("取消", eam::ui::icons::kClose)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (s_askDelete) { ImGui::OpenPopup("删除方案"); s_askDelete = false; }
        if (ImGui::BeginPopupModal("删除方案", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("删除方案“%s”？", s_active >= 0 && s_active < (int)names.size() ? names[s_active].c_str() : "");
            ImGui::TextDisabled("当前滑块设置保持不变。");
            ImGui::Dummy(ImVec2(0, 4));
            if (eam::ui::Button("删除", eam::ui::icons::kTrash, eam::ui::ButtonKind::Danger)) {
                { std::lock_guard<std::mutex> lk(g_settingsMutex); if (s_active >= 0 && s_active < (int)g_looks.size()) { ForgetLook(g_host, kAddonId, g_looks[s_active].name); g_looks.erase(g_looks.begin() + s_active); } }
                s_active = -1; changed = true; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (eam::ui::Button("取消", eam::ui::icons::kClose)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    }

    Block(kProductName);
    const bool dlaa = kScalerAddon;
    if (!kScalerAddon) {   // the upscalers are switched on and off in the addon list only (one switch, not two)
        const std::string label = std::string("启用 ") + kProductName;
        if (ImGui::Checkbox(label.c_str(), &c.enabled)) { changed = true; if (c.enabled) { ClaimFrames(); SwitchOn(); } else ReleaseFrames(); }
        Tip(kScalerAddon
                ? "总开关。关闭 = Lossless Scaling 的 NIS 照常运行，上采样器停止。\n想在游戏中对比，请改用前后对比快捷键：它会让上采样器保持运行。\n"
                  "多个上采样器（DLSS、FSR、XeSS）同时只能运行一个（在插件列表中开启一个会关闭其余）；它们都可以与 DLSS 5 神经渲染并存。"
                : "总开关。关闭 = Lossless Scaling 不受影响照常运行，模型停止。\n想在游戏中对比前后画面，请改用前后对比快捷键：它会让模型保持运行。");
    }
    if (!kScalerAddon) ImGui::SameLine();
    if (ImGui::SmallButton("重置历史")) g_resetRequested = true;
    Tip("模型会把当前帧与之前的帧混合。场景切换后，或屏幕上有残影或拖影卡住不动时，按此按钮。");
    ImGui::SameLine();
    if (ImGui::SmallButton("恢复默认值")) {
        const float was = c.p.workingScale; const NrParams keep = c.p; c.p = ProductDefaults(); c.p.hudCount = keep.hudCount; memcpy(c.p.hud, keep.hud, sizeof c.p.hud); c.p.hudFeather = keep.hudFeather; c.lsFirst = true; changed = true; if (c.p.workingScale != was) createChanged = true;
    }
    Tip("把方案和画质滑块恢复为本插件的默认值。你的预设、快捷键和高级设置不受影响。");
    if (!kScalerAddon) {   // the other addon of the pair (the upscalers take the NIS pass and work beside Neural Rendering)
        const std::string owner = FrameOwner();
        if (!owner.empty() && owner != kAddonId)
            Note("%s 目前已开启。开启此项会将其关闭：两者同时只能有一个处理画面。", ProductNameOf(owner.c_str()));
    }
    bool modelChanged = false;
    if (kScalerAddon)
    {   // which FSR (or DLSS) runs: the same choice as the + beside it in the manager's Runtimes list
        static std::vector<RuntimeChoice> choices; static ULONGLONG listedAt = 0;
        if (!listedAt || GetTickCount64() - listedAt > 3000) { choices = RuntimeChoices(); listedAt = GetTickCount64(); }
        const std::wstring chosen = ChosenRuntimeFile();
        int current = 0;
        for (size_t i = 1; i < choices.size(); ++i) if (_wcsicmp(choices[i].path.c_str(), chosen.c_str()) == 0) current = (int)i;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        if (!choices.empty() && ImGui::BeginCombo(kXessScaler ? "XeSS 版本" : kFsrScaler ? "FSR 版本" : "DLSS 版本", choices[current].name.c_str())) {
            for (size_t i = 0; i < choices.size(); ++i)
                if (ImGui::Selectable(choices[i].name.c_str(), (int)i == current) && (int)i != current) ChooseRuntimeFile(choices[i].path);
            ImGui::EndCombo();
        }
        Tip(kXessScaler ? "运行哪个 XeSS 运行时：插件自带的 Intel 版本，或通过管理器运行时列表（左下角）中 XeSS 旁的 + 添加的版本。"
                          "游戏运行中切换需要一点时间。"
          : kFsrScaler ? "运行哪个 FSR。插件自带 AMD 的 FSR 3.1.4，可在任何显卡上运行。FSR 4 是 AMD 较新的机器学习上采样器："
                         "更锐利、运动中更稳定，但开销更大；OptiScaler 团队的构建能在 AMD 官方不支持的显卡上运行它（该构建未签名）。"
                         "游戏运行中切换需要一秒。更多文件：管理器运行时列表（左下角）中 FSR 旁的 +。"
                       : "运行哪个 DLSS 运行时：插件自带的版本，或通过管理器运行时列表（左下角）中 DLSS 旁的 + 添加的版本。"
                         "游戏运行中切换需要一点时间。");
    }
    if (dlaa && !kAnyCardScaler) {
        // one line each: the letter, the generation, what it does best here (nr_sreval, Silent Hill f shrunk 1.5x, 2026-09-27: L the steadiest
        // in slow motion and close to the best in fast turns; E the best in fast turns; M the weakest here and the heaviest. World of Warcraft at
        // 1:1, 2026-09-28: L pulses on a 4-frame cycle, seen as flicker while moving; E does not, and the owner confirmed it live)
        static const char* const presets[] = { "自动（推荐）：1:1 时用 E；上采样时 RTX 20/30 显卡用 K，更新的显卡用 L", "K：DLSS 4，最轻量（NVIDIA 质量档的默认）", "L：DLSS 4，上采样时最稳定",
                                               "J：DLSS 4，拖尾比 K 略少", "M：DLSS 4.5，游戏中最锐利，开销最大", "E：DLSS 3（CNN），1:1 时最稳定（DLAA）" };
        static const unsigned kPresetOf[] = { SrEngine::kPresetAuto, 11u, 12u, 10u, 13u, 5u };
        int preset = 0;
        for (int i = 0; i < 6; ++i) if (kPresetOf[i] == c.dlaaPreset || (i == 1 && c.dlaaPreset == 0)) preset = i;   // (a saved 0, NVIDIA's own default, shows as K)
        if (ImGui::Combo("DLSS 模型", &preset, presets, 6)) { c.dlaaPreset = kPresetOf[preset]; changed = true; modelChanged = true; }
        Tip("运行哪个 DLSS 模型。推荐：自动，1:1 时选 E（DLAA：游戏以屏幕原始大小运行），上采样时选 L。1:1 时 L 每四帧脉动一次，"
            "移动时表现为闪烁（在《魔兽世界》中实测）；E 不会。上采样时，L 是唯一在镜头缓慢平移时不偏离画面的模型，"
            "快速转动时也接近最佳。K 是 NVIDIA 的默认。J 与 K 同源：拖尾略少，"
            "闪烁略多。M 是 DLSS 4.5 的模型：在提供相机抖动的游戏中更锐利，开销约为 K 的三倍；"
            "在这里表现最差。E 是 DLSS 3 的 CNN 模型：最轻量，快速转动时最佳，静态画面保持清晰。用前后对比快捷键"
            "对比两者。更改后会重启引擎。\n"
            "Lossless Scaling 不向 DLSS 提供相机抖动和深度信息，因此它能平滑稳定边缘与闪烁，但无法像在原生支持 DLSS 的游戏中那样添加画面本身之外的细节。");
    }

    Block("设置");
    Note("展开一个分区即可修改。滑块：双击重置，Ctrl+点击输入数值，Ctrl+滚动微调。小刻度标记默认值。");
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
    if (eam::ui::SectionHeader(dlaa ? "运动估计" : "模型（它对画面的处理）")) {
        // Read by the model at every evaluate: changes apply on the next frame. Ranges are what the model honours
        // (docs/dlssnr-knobs.md): intensity clamps at 1, the local strengths do not clamp at all.
        if (!dlaa) {
        int style = (int)c.p.style; const char* styles[] = { "标准", "自然", "电影感" };
        if (ImGui::Combo("风格", &style, styles, 3)) { c.p.style = style; changed = true; }
        Tip("模型的三种风格。标准为默认。自然会把高光压缩约 10%，看起来可能像暗角。电影感是第三种变体。");
        changed |= SL("模型强度", &c.p.intensity, 0.0f, 1.0f, "%.2f");
        Tip("模型最终呈现多少自身修改。0 = 模型不做任何更改；1 = 全部生效。模型上限为 1。\n（下方的混合幅度滑块是独立的：它缩放事后叠加到画面上的内容。）");
        changed |= SL("精细细节强度", &c.p.localStructure, -2.0f, 5.0f, "%.2f");
        Tip("模型对精细细节（边缘、纹理、文字）的处理强度。默认为 1。约 5 以上会变成乱码；负值会呈现“反向细节”效果。模型不对此做钳制。");
        changed |= SL("局部对比度", &c.p.localTone, -2.0f, 5.0f, "%.2f");
        Tip("模型对局部色调与对比度（明暗区域分离程度）的处理强度。默认为 1；范围特性与精细细节强度相同。");
        changed |= SL("皮肤与人脸细节", &c.p.skinStructure, -1.0f, 3.0f, c.p.skinStructure <= -0.99f ? "同精细细节" : "%.2f");
        Tip("模型识别为皮肤区域的细节强度。拉到最左端时直接跟随精细细节强度。只在检测到皮肤的地方生效，因此变化可能很细微。");
        bool am = c.p.useAutoMask != 0;
        if (ImGui::Checkbox("自动检测皮肤", &am)) { c.p.useAutoMask = am; changed = true; }
        Tip("让模型自行寻找皮肤和人脸（即“自动遮罩”）。效果较小；默认开启。");
        }
        if (dlaa) {   // the upscaler: where DLSS's motion vectors come from
            const char* sources[] = { "从帧间测量（任何游戏）", "Lossless Scaling 的帧生成", "无" };
            if (ImGui::Combo("运动估计", &c.motionSource, sources, 3)) changed = true;
            Tip(kXessScaler ? "XeSS 会结合多帧，需要知道每个像素在上一帧中的位置；内置 XeSS 的游戏会自行提供。此处可选：\n"
                           "从帧间测量：上采样器把当前帧与上一帧对比，找出画面各部分的移动方式。"
                           "帧生成开或关均可，任何游戏都适用。占用少量 GPU 时间（显示在上采样一节）。\n"
                           "Lossless Scaling 的帧生成：用其帧生成测得的运动（仅在帧生成开启时可用；较粗糙，为游戏画面的四分之一大小）。\n"
                           "无：XeSS 假定画面静止不动。静止时清晰，镜头转动时拖影：仅用于对比。" :
                kFsrScaler ? "FSR 会结合多帧，需要知道每个像素在上一帧中的位置；内置 FSR 的游戏会自行提供。此处可选：\n"
                           "从帧间测量：上采样器把当前帧与上一帧对比，找出画面各部分的移动方式。"
                           "帧生成开或关均可，任何游戏都适用。占用少量 GPU 时间（显示在上采样一节）。\n"
                           "Lossless Scaling 的帧生成：用其帧生成测得的运动（仅在帧生成开启时可用；较粗糙，为游戏画面的四分之一大小）。\n"
                           "无：FSR 假定画面静止不动。静止时清晰，镜头转动时拖影：仅用于对比。" :
                "DLSS 会结合多帧，需要知道每个像素在上一帧中的位置；内置 DLSS 的游戏会自行提供。此处可选：\n"
                "从帧间测量：上采样器把当前帧与上一帧对比，找出画面各部分的移动方式。"
                "帧生成开或关均可，任何游戏都适用。占用少量 GPU 时间（显示在上采样一节）。\n"
                "Lossless Scaling 的帧生成：用其帧生成测得的运动（仅在帧生成开启时可用；较粗糙，为游戏画面的四分之一大小）。\n"
                "无：DLSS 假定画面静止不动。静止时清晰，镜头转动时拖影：仅用于对比。");
        }
        if (!dlaa) {
        changed |= ImGui::Checkbox("使用 Lossless Scaling 的运动数据", &c.p.useFlow);
        Tip("把 Lossless Scaling 帧生成测得的运动（其光流）作为运动向量提供给模型，并用它把增强效果对齐到生成的中间帧上。仅在没有运动的情况下测试时才关闭。");
        { const NrStats& fs = g_engine.Stats(); ImGui::SameLine(); if (fs.hasFlow) ImGui::TextDisabled("（光流 %ux%u）", fs.flowW, fs.flowH); else ImGui::TextDisabled("（尚未见到光流纹理）"); }
        {
            int mm = static_cast<int>(c.p.modelMotion); const char* mms[] = { "从帧间测量（逐像素）", "Lossless Scaling 的光流（粗糙）" };
            if (!c.p.useFlow) ImGui::BeginDisabled();
            if (ImGui::Combo("模型的运动", &mm, mms, 2)) { c.p.modelMotion = static_cast<uint32_t>(mm); changed = true; }
            if (!c.p.useFlow) ImGui::EndDisabled();
            Tip("模型的运动向量来自哪里。从帧间测量（默认）：插件把模型看到的每一帧与上一帧对比，"
                "逐像素找出本帧各部分的移动，让模型输出在运动中保持连贯、拖影更少。"
                "Lossless Scaling 的光流：其帧生成测得的运动，为画面四分之一大小。无论选哪种，增强效果都会"
                "通过 Lossless Scaling 的光流对齐到生成的帧上。在模型工作分辨率下，测量耗时不到一毫秒。");
        }
        if (!c.p.useFlow) ImGui::BeginDisabled();
        if (ImGui::Checkbox("使用本帧的运动（等待 Lossless Scaling 的光流）", &c.freshFlow)) { changed = true; tapChanged = true; }
        Tip("开：模型在每帧中稍晚运行，等 Lossless Scaling 测得本帧的移动后才执行，因此运动向量是最新的。"
            "关：帧一捕获就运行，拿到的是上一帧的运动，晚了一帧，镜头转动、启动或停止时会出现拖影和残影。"
            "默认为开；提供此开关是为了对比两种模式。");
        if (!c.p.useFlow) ImGui::EndDisabled();
        if (ImGui::Checkbox("画面铺满整个屏幕", &c.composeFillsScreen)) changed = true;
        Tip("关（默认）：屏幕比游戏更宽或更高时（如 32:9 屏幕上运行 16:9 游戏），Lossless Scaling 会按游戏原始比例绘制并留黑边，"
            "模型的修改只作用于画面而不触及黑边。若 Lossless Scaling 设置为把画面拉伸到整个屏幕，请开启此项，让模型的修改覆盖全屏。");
        if (ImGui::Checkbox("帧生成关闭时也运行", &c.presentMode)) changed = true;
        Tip("Lossless Scaling 的帧生成关闭时，没有可用的捕获帧。开（默认）：模型改用 Lossless Scaling 呈现的帧。"
            "模型工作分辨率按 1920 宽的帧计算，因此开销与帧生成开启时相同。"
            "关：没有帧生成时插件不做任何事。");
        if (!c.presentMode) ImGui::BeginDisabled();
        ImGui::Indent();
        if (ImGui::Checkbox("等待每帧自己的结果", &c.presentWait)) changed = true;
        Tip("关（默认）：每帧使用已就绪的最新结果，通常是上一帧的结果，按测得的运动对齐到当前画面位置。"
            "无需等待，因此模型耗时不会加在每帧前面。开：每帧在 GPU 上等待自己的结果。精确，"
            "但模型的完整运行（数毫秒）会排在每帧显示之前，可能导致帧错过显示器的刷新。");
        ImGui::Unindent();
        if (!c.presentMode) ImGui::EndDisabled();
        {
            static const char* const encodings[] = { "自动", "SDR", "HDR" };
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("帧编码", &c.frameEncoding, encodings, 3)) changed = true;
            Tip("帧的内容格式，让 HDR 画面按模型和方案控件的预期处理。自动（默认）：8 位帧视为"
                "SDR，16 位浮点帧视为 HDR (scRGB)，10 位帧在 Windows 以 HDR 运行显示器时视为 HDR (HDR10)，否则视为 SDR。HDR 帧"
                "会以 Windows 的 SDR 内容亮度作为白色基准转到 SDR 视图，且只有修改部分写回帧中，因此高光"
                "保持原有亮度。此处选 SDR 或 HDR 会覆盖 10 位和 16 位帧的判定（8 位帧始终是 SDR）："
                "仅在画面发灰、过暗或过亮时才需要设置。技术状态一节会显示实际判定结果。");
        }
        }
    }
    if (kScalerAddon && eam::ui::SectionHeader("上采样")) {
        const ScalerView v = GetScalerView();
        const char* const U = kUpscalerName;
        if (v.starting) ImGui::TextDisabled(kXessScaler ? "正在加载 Intel 的 XeSS 运行时..." : kFsrScaler ? "正在加载 AMD 的 FSR 运行时..." : "正在加载 NVIDIA 的 DLSS 运行时...");
        else if (v.failed) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kDanger), "%s 无法运行：%s。Lossless Scaling 的 NIS 照常运行。", U, v.error.c_str());
        else if (v.preparing) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kWarn), "%s 正在为该帧格式做准备（第一帧可能较慢）；期间由 NIS 运行。", U);
        else if (!v.blocked.empty()) {   // the NIS pass is there but the upscaler cannot take it: say why, rather than look like it runs
            ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn));
            if (v.nisSeen) ImGui::TextWrapped("找到 NIS 处理趟（%ux%u -> %ux%u），但 %s 未接管它：%s", v.inW, v.inH, v.outW, v.outH, U, v.blocked.c_str());
            else ImGui::TextWrapped("%s 未接管 NIS：%s", U, v.blocked.c_str());
            ImGui::PopStyleColor();
        }
        else if (!v.nisSeen) ImGui::TextWrapped("正在等待 Lossless Scaling 的 NIS 处理趟。请选择 NIS 作为缩放类型，并缩放一个以小于屏幕的窗口运行的游戏。");
        else if (!v.ready) ImGui::TextDisabled("找到 NIS 处理趟（%ux%u -> %ux%u）；%s 尚未运行。", v.inW, v.inH, v.outW, v.outH, U);
        else {
            const std::string running = v.provider.empty() ? std::string(U) : std::string(U) + " " + v.provider;   // "FSR 4.1.1b"
            ImGui::TextWrapped("%s 接替 NIS 上采样 %ux%u -> %ux%u (x%.2f)：GPU 上每帧 %.2f ms（其中运动估计 %.2f ms），目前已处理 %llu 帧%s。", running.c_str(), v.inW, v.inH,
                               v.outW, v.outH, v.inW ? (float)v.outW / v.inW : 0.0f, v.gpuMs, v.motionMs, (unsigned long long)v.runs,
                               v.perFrame > 1 ? "（真实帧与生成帧同样处理）" : "");
            if (v.second.valid) {
                ImGui::TextDisabled("上一秒：%.0f 幅/秒（%.0f%% 为紧密帧对）；%.1f%% 重复显示，%.1f%% 的帧未交付，%.1f%% 在 GPU 上等待。",
                                    v.second.fps, v.second.closePct, v.second.repeatPct, v.second.skipPct, v.second.waitPct);
                Tip("紧密帧对：彼此相差不到 6 ms 的两帧，由自适应帧生成产生。重复显示：Lossless Scaling 需要画面时"
                    "上采样器还没完成更新的一幅，于是又显示了上一幅（轻微卡顿）。在 GPU 上等待：为避免重复，"
                    "Lossless Scaling 的帧在 GPU 上等待下一幅画面（即下方那个设置）。未交付：上采样器还有两帧待处理，"
                    "于是跳过了这一帧。GPU 满载时这些数值会升高：设置一个略有余量的帧率上限最有效。");
            }
            if (g_compare.load() == 2) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kWarn), "正在显示 Lossless Scaling 的 NIS 以供对比（前后对比快捷键）。");
        }
        {   // a preset for how much work the upscaler spends: the resolution ratio is the game window's (Lossless Scaling scales what it captures), so these set the extras
            struct Preset { bool skipRepeats, lightAuto, lightGen, reuseMotion; float steadySharp; };
            static const Preset presets[3] = { { false, false, false, false, 0.6f },    // Quality: every picture in full, a motion estimate for each
                                               { false, true,  true,  true,  0.6f },    // Balanced (the defaults)
                                               { true,  true,  true,  true,  0.0f } };  // Performance: repeats skipped, generated frames always lighter, the motion estimate shared, no steady sharpening
            int current = 3;
            for (int i = 0; i < 3; ++i) {
                const Preset& q = presets[i];
                if (c.scalerSkipRepeats == q.skipRepeats && c.scalerLightAuto == q.lightAuto && c.scalerLightGen == q.lightGen && c.scalerReuseMotion == q.reuseMotion && std::abs(c.scalerSteadySharp - q.steadySharp) < 0.005f) { current = i; break; }
            }
            int pick = current;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
            if (ImGui::Combo("预设", &pick, "质量\0平衡（默认）\0性能\0自定义\0") && pick < 3 && pick != current) {
                const Preset& q = presets[pick];
                c.scalerSkipRepeats = q.skipRepeats; c.scalerLightAuto = q.lightAuto; c.scalerLightGen = q.lightGen; c.scalerReuseMotion = q.reuseMotion; c.scalerSteadySharp = q.steadySharp;
                changed = true;
            }
            Tip("上采样器花费多少算力。游戏画面与屏幕的比例不在这里设置：它取决于游戏窗口相对屏幕的大小（窗口为屏幕一半就是"
                "其他上采样器所说的性能档）。质量：每幅画面都完整上采样，每幅都有自己的运动估计。平衡：默认值。性能：与上一帧完全相同的"
                "画面不再重复上采样，帧生成的生成帧总是用更轻量的上采样，运动估计在它们之间共享，稳定锐化"
                "关闭：最省算力，适合较弱的副卡。修改下方任何一项都会变成自定义。");
        }
        changed |= SL("锐化", &c.p.sharpen, 0.0f, 1.0f, c.p.sharpen <= 0.001f ? "关" : "%.2f");
        if (kFsrScaler) Tip("对上采样后画面的锐化，由我们自己的处理趟完成，与 DLSS 和 XeSS 上一样（AMD 的 RCAS 保留的细节太少）。"
                            "Lossless Scaling 的 NIS 也有锐化（其锐度设置），所以不开启时 FSR 在 NIS 旁边会显得更柔。0.5 是不错的起点（游戏中按 Ctrl+Shift+F2 / F3）。");
        else Tip("对 DLSS 画面的对比度自适应锐化（FidelityFX CAS 公式），耗时不到一毫秒；约 0.6 以上时，效果会在 CAS 自身上限之外继续增强。"
                 "DLSS 自身没有锐化，而 Lossless Scaling 的 NIS 有（其锐度设置），所以不开启时 DLSS 在 NIS 旁边会显得更柔。"
                 "0.5 是不错的起点（游戏中按 Ctrl+Shift+F2 / F3）。");
        // FSR 4 keeps its history its own way and takes none of FSR 3.1's tuning (nor the mask the slider widens): the slider would do nothing
        const bool fsr4 = kFsrScaler && GetScalerView().provider.rfind("4", 0) == 0;
        if (fsr4) ImGui::BeginDisabled();
        { const float off = 0.0f; changed |= eam::ui::SliderFloat("稳定性", &c.scalerStability, 0.0f, 1.0f, c.scalerStability <= 0.001f ? "关" : "%.2f", 0, &off); }
        if (fsr4) { ImGui::EndDisabled(); Note("FSR 4 自行处理稳定性：此滑块仅用于 FSR 3.1。"); }
        { char tip[768];
          snprintf(tip, sizeof tip, "减少细线、电线和树叶上的闪烁，代价是移动物体身后拖尾更多。游戏不向上采样器提供亚像素相机抖动，"
                   "因此比一个像素还细的线会在帧间闪烁；调高此项后，运动测量不再把这种闪烁当作不可信历史的位置报告，"
                   "于是 %s 会在之前的几帧间把它平均掉%s。关闭：与从前一样。试试 0.5，然后一边盯着栅栏或电线一边调整。"
                   "细线本来仍由游戏自身的抗锯齿（MSAA）绘制。",
                   U, kFsrScaler ? "，且 FSR 会保留更多历史信息，对明暗的细微变化反应更小" : "");
          Tip(tip); }
        {
            if (ImGui::Checkbox("快速运动中保持稳定", &c.scalerFastMotion)) changed = true;
            Tip("开（默认）：画面快速移动（快速转动镜头）或运动无法跟随时，上采样器画面倚重新帧而非之前的帧。"
                "游戏不向上采样器提供亚像素相机抖动，它对早先帧的记忆在那里加不了多少细节，"
                "快速转动时还会拖尾：树叶和边缘出现柔化的双重轮廓。关：与从前一样。"
                "转动镜头时切换对比。");
            if (c.scalerFastMotion) {
                if (ImGui::SliderFloat("倾斜起点", &c.scalerLeanFrom, 0.0f, 0.5f, c.scalerLeanFrom <= 0.0f ? "自动" : "每帧画面宽度的 %.2f %%")) changed = true;
                Tip("开始倾斜的运动量门槛（达到其两倍时完全倾斜）。自动（0）：1:1 时每帧为画面宽度的 0.1 %（4K 下约 4 px），"
                    "上采样时为 0.05 %。调低：移动时拖尾和涂抹更少，但 1:1 时游戏自身的锯齿在运动中更明显。"
                    "调高：运动中上采样器的平滑更多，拖尾也更多。边走边转动镜头时调整。");
            }
            if (ImGui::SliderFloat("静止时锐度", &c.scalerLeanRest, 0.0f, 1.0f, c.scalerLeanRest <= 0.0f ? "关" : "%.2f")) changed = true;
            Tip("画面静止时显示的是上采样器自己的画面，它比游戏的更柔：没有 Lossless Scaling 提供的相机抖动，它找不到额外细节，"
                "只有自身的平滑（抗锯齿）可给。此项让普通拉伸画面（见快速运动中保持稳定）在静止时也按此比例混入：0 为纯上采样器，1 为纯拉伸。"
                "静态画面看起来发软时调高；锐化（上方）会在此基础上再叠加。默认为 0.5；"
                "与游戏的原始大小帧相比，它比 0 更接近原画面也更稳定。");
            if (ImGui::SliderFloat("稳定锐化", &c.scalerSteadySharp, 0.0f, 0.9f, c.scalerSteadySharp <= 0.0f ? "关" : "%.2f")) changed = true;
            Tip("锐化在放大细节的同时也会放大游戏自身的闪烁。开启后，锐化量根据画面的滑动平均计算"
                "（沿运动跟踪，移动快处可信度降低），只有这部分被加到当前帧上：细节保留，闪烁不被放大。"
                "在录制画面上，静态场景下同等细节约减少 10 点闪烁，上采样运动中则无变化。"
                "4K 下耗时约一毫秒。0 为关闭；默认 0.6。");
            if (ImGui::SliderFloat("快速运动中减少锐化", &c.scalerMoveCut, 0.0f, 1.0f, c.scalerMoveCut <= 0.0f ? "关" : "%.2f")) changed = true;
            Tip("画面快速移动处，锐化添加的额外细节肉眼分辨不出，但它放大的闪烁却看得见。"
                "此项把锐化从每帧 1 到 8 像素的运动量起平滑降低，最快时降这么多；静止和慢速运动时不受影响。"
                "在一段以 1:1 行走在开裂地面上的录制中，0.5 把锐化新增的闪烁从游戏自身的 113 % 降到 107 %，细节从 137 % 降到 127 %；"
                "1.0 则降到 102 % 和 117 %。与稳定锐化的处理趟配合（它需要运动估计）。0 为关闭；默认 0.5。");
        }
        {
            if (ImGui::Checkbox("运动中边缘更清晰（测试）", &c.leanEasu)) changed = true;
            Tip("一项测试，默认关闭。画面移动处，上采样画面倚重新帧，拉伸到屏幕大小（见快速运动中保持稳定）。"
                "关：用平滑滤波（Catmull-Rom）。开：用 AMD 的 FSR 1 边缘自适应滤波，斜线和细边缘更干净锐利。"
                "在录制画面上两者闪烁和细节得分相同，差别在边缘观感。"
                "转动镜头时切换对比，告诉我们你更喜欢哪种。");
        }
        {
            if (ImGui::Checkbox("生成帧之间共享运动估计", &c.scalerReuseMotion)) changed = true;
            Tip("帧生成时，游戏每绘制一帧就显示两帧，而这两步之间到下一帧的运动大致相同。"
                "开：隔一帧沿用前一帧的运动而不重新测量，4K 下每帧约省四分之一毫秒，录制得分相同。"
                "关：每帧都重新测量。帧生成关闭时无作用。");
        }
        {
            if (ImGui::Checkbox("与上一幅完全相同的画面不再上采样（测试）", &c.scalerSkipRepeats)) changed = true;
            Tip("一项测试，默认关闭。在显卡上把每幅画面与上一幅精确对比；相同时（暂停菜单、加载画面，或通过采集卡以 60 或 120 Hz 显示的 30 fps 游戏），直接复制上一次上采样的画面而不重新上采样，把上采样器对这幅画面的全部开销（DLSS 在 2560x1440 下约 1.4 ms GPU）降到一次对比加一次复制。每第八次重复会真正上采样，所以你改动的设置几幅画面内就会显示。若显卡太忙没来得及应答对比，画面照常上采样；连续三次应答过慢后，对比会暂停十秒。经录制实测，画面与不开启时相同。");
            if (c.scalerSkipRepeats && v.ready && v.repeatChecks > 0) ImGui::TextDisabled("  %llu / %llu 幅对比的画面与上一幅相同，未再次上采样（%.0f %%）", static_cast<unsigned long long>(v.repeatSkips),
                                                                                       static_cast<unsigned long long>(v.repeatChecks), 100.0 * v.repeatSkips / v.repeatChecks);
            if (ImGui::Checkbox("显卡满载时生成帧用更轻的上采样", &c.scalerLightAuto)) changed = true;
            Tip("默认开启。帧生成时上采样器会处理每幅显示的画面，包括生成的画面，因此重模型（DLSS 4.5、FSR 4）可能把游戏帧率拉低得远比轻模型多。此项每秒读取一次显卡负载；当连续三次读到负载达到或超过上限（管理器功能页的 GPU 限制，未设置时为 95 %），Lossless Scaling 在游戏自身帧之间生成的帧就改用下述更轻量的画面，负载连续十秒明显低于上限后再恢复完整上采样。经录制实测，更轻量的画面代价约 0.1 至 0.3 dB（快速运动中略多，1.5 dB），把这些帧的上采样开销从约 1.4 ms 降到 0.2 ms 以下（2560x1440）。帧生成关闭时无作用，显卡负载不可读时也无作用。关闭此项则始终完整上采样每幅画面。");
            if (ImGui::Checkbox("生成帧用更轻的上采样", &c.scalerLightGen)) changed = true;
            Tip("默认开启。帧生成时，上采样器只处理游戏绘制的帧；Lossless Scaling 在其间生成的帧改用上一幅上采样画面沿运动移动、与新帧核对"
                "（不一致处用新帧直接拉伸），再锐化。4K 下这把这些帧的上采样开销从约 3 ms 降到 1 ms 以下，即游戏帧开销的四分之一。"
                "录制实测细节和闪烁得分相同，运动中的稳定性略低。关闭此项则每帧都完整上采样（质量预设即如此）。"
                "帧生成关闭时无作用。");
        }
        {
            if (ImGui::Checkbox("按形状测运动（测试）", &c.motionShapes)) changed = true;
            Tip("一项测试，默认关闭。提供给上采样器的运动，是把上一帧的小块区域与当前帧匹配得出的。关：按亮度匹配，与从前一样。"
                "开：按形状和边缘匹配，亮度本身权重很小，这样闪光、法术特效、淡入淡出或掠过的阴影更不容易误导它。"
                "在移动中光线会变化的地方切换对比，告诉我们哪个看起来更稳。");
        }
        { const float off = 0.0f; changed |= eam::ui::SliderFloat("边缘平滑", &c.scalerEdges, 0.0f, 1.0f, c.scalerEdges <= 0.001f ? "关" : "%.2f", 0, &off); }
        Tip("沿上采样画面边缘的抗锯齿：面向没有自带抗锯齿的游戏（屋顶、栅栏和电线上的锯齿）。"
            "它找出亮度阶跃的位置、边缘的走向和长度，按真实边缘会覆盖的像素比例进行混合。"
            "耗时不到一毫秒。它也会让文字略微变柔。"
            "游戏自带抗锯齿（MSAA）时用它更好：此项留给没有抗锯齿的游戏。");
        {   // the picture controls: on the frame before it is upscaled; Neural Rendering's own take over while it is on
            const bool nrOn = NeuralRenderingOn();
            if (nrOn) ImGui::BeginDisabled();
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("自然饱和度", &c.p.vibrance, 0.0f, 1.0f, c.p.vibrance <= 0.001f ? "关" : "%.2f", 0, &d); }
            Tip("对黯淡颜色的提升远大于鲜艳颜色，因此肤色和本就浓郁的颜色不会被继续推高：自然的增色。");
            { const float d = 1.0f; changed |= eam::ui::SliderFloat("饱和度", &c.p.saturation, 0.0f, 2.0f, std::abs(c.p.saturation - 1.0f) < 0.005f ? "不变" : "%.2f", 0, &d); }
            Tip("整体增加（大于 1）或减少（小于 1，0 为黑白）颜色。");
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("阴影", &c.p.shadows, -1.0f, 1.0f, std::abs(c.p.shadows) < 0.005f ? "不变" : "%+.2f", 0, &d); }
            Tip("只提亮（右）或加深（左）暗部：看清黑暗的室内或夜晚，而其余部分不发灰。");
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("高光", &c.p.highlights, -1.0f, 1.0f, std::abs(c.p.highlights) < 0.005f ? "不变" : "%+.2f", 0, &d); }
            Tip("只压制（左）或提亮（右）亮部：过曝的天空或雪地找回细节。");
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("亮度", &c.p.brightness, -0.3f, 0.3f, "%+.2f", 0, &d); }
            Tip("整体提亮或压暗画面，如同显示器的亮度。用于太暗（或太亮）看不清的游戏。");
            { const float d = 1.0f; changed |= eam::ui::SliderFloat("对比度", &c.p.contrast, 0.5f, 1.5f, "%.2f", 0, &d); }
            Tip("以中间灰为中心，把色调拉开（大于 1）或收拢（小于 1）。");
            { const float d = 1.0f; changed |= eam::ui::SliderFloat("Gamma", &c.p.gamma, 0.5f, 2.0f, "%.2f", 0, &d); }
            Tip("大于 1 时提亮中间调，黑与白保持不动：暗角变得可见而天空不发白。"
                "小于 1 则压暗中间调。");
            if (nrOn) { ImGui::EndDisabled(); Note("DLSS 5 神经渲染已开启：现在由其画面分区控制这些选项。"); }
        }
        {
            std::string game; { std::lock_guard<std::mutex> lk(g_settingsMutex); game = g_scalerGame; }
            if (ImGui::Checkbox("每个游戏单独保存这些设置", &c.scalerPerGame)) changed = true;
            Tip("开（默认）：锐化、稳定性、边缘平滑、颜色与色调控制、DLSS 模型和运动设置按游戏分别保存，该游戏获得焦点时自动恢复。"
                "此处修改对最近游玩的游戏生效。");
            if (c.scalerPerGame) {
                ImGui::SameLine();
                ImGui::TextDisabled(game.empty() ? "（尚未见到游戏）" : "当前游戏：%s", game.c_str());
                if (!c.scalerGames.empty()) {
                    int forget = -1;
                    if (ImGui::TreeNode("有独立设置的游戏")) {
                        for (size_t i = 0; i < c.scalerGames.size(); ++i) {
                            const auto& [exe, p] = c.scalerGames[i];
                            ImGui::PushID(static_cast<int>(i));
                            ImGui::AlignTextToFramePadding();
                            ImGui::Text("%s：锐化 %.2f，稳定性 %.2f，边缘 %.2f", exe.c_str(), p.sharpen, p.stability, p.edges);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("清除")) forget = static_cast<int>(i);
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                    if (forget >= 0) { c.scalerGames.erase(c.scalerGames.begin() + forget); changed = true; }
                }
            }
        }
        if (ImGui::Checkbox("在 GPU 上等待而不是重复上一幅画面", &c.scalerGpuWait)) changed = true;
        Tip("当 Lossless Scaling 需要画面而上采样器还没完成新画面时（两帧靠得很近，自适应帧生成就会这样），会再次显示上一幅画面：轻微卡顿。"
            "开（默认）：Lossless Scaling 的帧改为在 GPU 上等待新画面，如同内置 DLSS 的游戏等待 DLSS；CPU 永不等待。"
            "关：再次显示上一幅画面。开关此项，对比上面一行的数值。");
        Note("用前后对比快捷键（对比与快捷键一节）对比：游玩时它在 %s 与 Lossless Scaling 自带的 NIS 之间切换。", U);
        {   // HDR games: the frames are upscaled as light in the upscaler's HDR mode and put back in their own encoding (scaler11.h, SetEncoding)
            static const char* const encodings[] = { "自动", "SDR", "HDR" };
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("帧编码", &c.frameEncoding, encodings, 3)) changed = true;
            Tip("面向 HDR 游戏。自动（默认）会自行判断：8 位帧为 SDR，16 位浮点帧为 HDR (scRGB)，"
                "10 位帧在 Windows 以 HDR 运行显示器时为 HDR (HDR10)。仅当画面发灰、过暗或过亮时才设置 SDR 或 HDR。"
                "技术状态一节会显示实际判定。");
        }
    }
    if (!kScalerAddon && eam::ui::SectionHeader("画质与性能")) {
        // The model costs ~10 ms + ~7 ms per megapixel on Ampere. The working scale is the only cost lever: past the
        // frame interval the model simply skips frames and the present side carries the last delta forward.
        if (dlaa) Note("DLAA 作用于整个画面，因此没有模型分辨率可选。");
        else {
        createChanged |= SL("模型分辨率", &c.p.workingScale, 0.25f, 1.0f, "%.2f x 画面");
        Tip("模型看到画面之前先按此比例缩小，模型的修改再拉伸回完整画面。1.00 = 模型看到整个画面：细节最佳，开销最大。这是唯一改变开销的设置。\n在 RTX 4070 Ti SUPER 上模型耗时约 2.7 ms 加每百万像素 1.8 ms。\n更改后模型会在后台重新构建：不到一秒内继续显示上一次结果，游戏不会卡住。\n开启自动画质时，此值是它使用的上限。");
        changed |= ImGui::Checkbox("自动画质：把模型限制在时间预算内", &c.autoQuality);
        Tip("当模型耗时超过下方预算时（场景复杂、显卡过热），降低模型分辨率；有余量时再回升到接近你上方的设置。它变化缓慢（超预算 3 秒后才下调，远低于预算 10 秒后才上调），因此依据的是稳定数值而非瞬时尖峰。不影响任何改变画面效果的设置。");
        if (c.autoQuality) {
            changed |= SL("时间预算", &c.autoBudgetMs, 2.0f, 15.0f, "%.1f ms");
            Tip("模型耗时要控制的时长。帧时间的一半是不错的起点：60 fps 时 8 ms，100 fps 时 5 ms。");
            changed |= SL("最低模型分辨率", &c.autoFloor, 0.25f, 1.0f, "%.2f x 画面");
            Tip("自动画质绝不会低于此值，即使模型仍超出预算。");
            {   // the manager's GPU limit (Features tab) works through this auto quality
                const uint32_t limit = g_host && g_host->GetHostVersion() >= 0x010300 ? g_host->GpuLimitPercent() : 0u;
                if (limit) {
                    const unsigned load = g_gpuPercent.load();
                    bool over; { std::lock_guard<std::mutex> lock(g_autoMutex); over = g_auto.GpuOver(); }
                    if (load) ImGui::TextWrapped("管理器的 GPU 限制已开启（%u %%）：显卡当前负载 %u %%。%s", limit, load, over ? "已超限：正在削减模型。" : "");
                    else ImGui::TextWrapped("管理器的 GPU 限制已开启（%u %%）；尚未读到显卡负载（模型运行时每秒读取一次）。", limit);
                } else ImGui::TextDisabled("要让显卡保持在负载上限之下，请在管理器功能页开启 GPU 限制：之后显卡达到限制时，自动画质会降低模型分辨率。");
            }
            float scale, avg; std::deque<AutoQuality::Step> history;
            { std::lock_guard<std::mutex> lock(g_autoMutex); scale = g_auto.Scale(); avg = g_auto.AverageMs(); history = g_auto.History(); }
            if (scale <= 0) ImGui::TextDisabled("自动画质将在模型首次运行后开始。");
            else if (scale < c.p.workingScale - 0.001f) ImGui::TextWrapped("自动画质正以 %.2f x 运行模型（模型 %.1f ms，预算 %.1f ms）。", scale, avg, c.autoBudgetMs);
            else ImGui::TextWrapped("自动画质正按你的设置 %.2f x 运行模型（模型 %.1f ms，预算 %.1f ms）。", scale, avg, c.autoBudgetMs);
            if (!history.empty() && ImGui::TreeNode("autohist", "最近的调整（%d）", static_cast<int>(history.size()))) {
                const uint64_t now = GetTickCount64();
                for (auto it = history.rbegin(); it != history.rend(); ++it)
                    ImGui::Text("%3llu 秒前：%.2f -> %.2f（模型 %.1f ms）", static_cast<unsigned long long>((now - it->atMs) / 1000), it->from, it->to, it->modelMs);
                ImGui::TreePop();
            }
        }
        }
        {
            static const char* const everyNames[] = { "每帧运行", "每 2 帧运行", "每 3 帧运行", "每 4 帧运行" };
            int every = std::clamp(c.modelEvery, 1, 4) - 1;
            if (ImGui::Combo("模型运行频率", &every, everyNames, 4)) { c.modelEvery = every + 1; changed = true; }
            Tip("模型在游戏真实帧上的运行频率。模型开销基本固定（快速显卡上每次约 3 ms，与模型分辨率无关），所以每 2 帧运行一次能省回约一半：中间的帧用上一次结果沿运动移动来显示，帧生成生成的帧本来就是这么做。在四段《魔兽世界》录制上离线实测：两帧前的结果沿运动移动后，与该帧自身结果的差距比一帧前的结果远 0.04 到 0.23 级（共 255 级），而结果本身有 4 到 8 级。自动画质可能在此之上要求更多。");
        }
        {
            const NrStats& st = g_engine.Stats();
            if (g_bridge.Width()) {
                float mp = (float)st.workW * (float)st.workH / 1e6f; double iv = g_bridge.IntervalMs();
                uint64_t runs = g_bridge.Runs(), skipped = g_bridge.Skipped(), seen = runs + skipped;
                ImGui::TextWrapped("帧 %ux%u -> 模型输入 %ux%u（%.2f MP）：模型 %.1f ms（平均 %.1f），帧间隔 %.1f ms", g_bridge.Width(), g_bridge.Height(), st.workW, st.workH, mp, st.nrMs, g_avgModelMs, iv);
                if (st.workW && st.workW < g_bridge.Width()) ImGui::TextDisabled("模型在比帧更小的画面上工作，其修改会被拉伸回完整画面：这个大小由模型分辨率或自动画质为符合预算而选定，并非插件限制（调高该设置或关闭自动画质可获得更大画面；开销也更大）。");
                if (st.builds) ImGui::TextWrapped("模型已构建 %u 次，最后一次耗时 %.0f ms（不在帧路径上）；期间 %llu 帧未运行%s", st.builds, st.builds == 1 ? "" : "s", st.lastBuildMs, (unsigned long long)st.busySkips);
                ImGui::TextWrapped("模型跟上了 %llu / %llu 帧（%.0f%%）；提交后 GPU 启动 +%.1f / 完成 +%.1f ms；截取 CPU %.2f ms", (unsigned long long)runs, (unsigned long long)seen, seen ? 100.0 * runs / seen : 0.0, st.startMs, st.doneMs, g_bridge.CpuMs());
                ImGui::TextWrapped("呈现：%llu 次在 LS 的交换链上（%s），合成 %llu 次，合成 CPU %.2f ms，目标 %s；最新增量 = 帧 %llu，应用于 %.2f 帧偏移处",
                    (unsigned long long)g_lsPresents, g_tap.PresentPattern(), (unsigned long long)g_composed, g_compose.CpuMs(), g_compose.TargetInfo(), (unsigned long long)g_lastDelta, g_lastOffset);
                if (seen > 30 && runs * 2 < seen) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kWarn), "模型在不到一半的帧上运行：增量由光流跨帧传递。调低模型分辨率可获得更新的结果。");
            } else ImGui::TextDisabled("（尚未截取到帧）");
        }
        if (!dlaa) {
        { int ps = (int)c.p.passes; const int dps = (int)kDefaults.passes; if (eam::ui::SliderInt("模型处理趟", &ps, 1, 4, "%d", 0, &dps)) { c.p.passes = (uint32_t)ps; changed = true; } }
        Tip("模型对每帧重复加工的次数；每个处理趟把上一个结果作为输入。1 = 正常。2 到 4 会让效果更强（也可能开始显得过度加工，请用前后对比快捷键对比）。\n每个额外处理趟约多一次模型运行：留意模型耗时和下方“跟上了”一行。模型跟不上时会跳帧，沿用上一次结果。");
        { int mp = (int)std::min<uint32_t>(c.p.modelPreset, 1u); const char* presets[] = { "较新（更小、更快）", "较旧（原始效果）" };
          if (ImGui::Combo("模型预设", &mp, presets, 2)) { c.p.modelPreset = (uint32_t)mp; changed = true; } }
        Tip("运行时使用哪个模型。自 310.9.16 构建起，较新的是运行时自身的选择：更小、约快 1.7 倍，效果更温和（约相当于旧模型 0.7 个处理趟）。较旧的是 310.8 及更早构建的模型：更强、原始的效果，开销也是旧的。\n早于 310.9.16 的模型文件只有旧模型，此项在那里不起作用。更改会重建模型（片刻显示旧结果）。");
        }
        changed |= SL("时间平滑", &c.p.deltaSmooth, 0.0f, 0.9f, c.p.deltaSmooth <= 0.001f ? "关" : "%.2f");
        Tip("把模型对本帧的修改与对上一帧的修改混合，并按 Lossless Scaling 的运动数据随画面移动。它能平息精细细节（远处道路、栅栏、植被）的闪烁和爬行，代价是镜头快速移动时略微变柔或残影。0 = 关；0.4 是默认（《魔兽世界》中的选定值）。开销几乎为零。");
        changed |= ImGui::Checkbox("让 Lossless Scaling 优先使用 GPU", &c.lsFirst);
        Tip("在共享显卡上把 Lossless Scaling 自身的图形工作排到模型之前，使模型运行时帧生成和呈现不被延误。推荐开启。");
        changed |= SL("混合幅度", &c.p.composeIntensity, 0.0f, 2.0f);
        Tip("模型修改有多少加到每幅呈现的帧上。1 = 恰好模型所作；0 = 无；大于 1 则夸大。");
        changed |= SL("残影防护", &c.p.ghostGuard, 0.0f, 1.0f, c.p.ghostGuard <= 0.001f ? "关" : "%.2f");
        Tip("阻止移动事物身后拖着的上一帧淡影。模型在较早的帧上工作，其修改借助 Lossless Scaling 的运动数据搬到当前帧；运动数据不可靠处（移动物体的边缘、刚露出的区域），修改会落错位置。此项在那里把修改淡出，越旧淡得越多，静止和匀速移动的区域不受影响。0 = 关，0.5 = 不错的起点，1 = 最强。想看它在哪里起作用，把诊断视图设为残影防护：变暗的区域即被淡出处。");
        changed |= SL("暗部防护", &c.p.darkGuard, 0.0f, 0.15f, c.p.darkGuard <= 0.0005f ? "关" : "%.3f");
        Tip("模型会提亮暗部，并在明亮物体周围留下淡淡光晕，例如黑底屏幕上的 HUD 框，在黑色上偏绿。比此值暗的像素完全不参与（它渐变到该值才完全生效；0.04 约为 255 级中的 10 级）。调高可去除更多暗色面板上的光晕，代价是模型对真实阴影的提亮；0 为关闭。");
        changed |= SL("限制单像素变化", &c.p.maxDelta, 0.05f, 1.0f);
        Tip("任何像素颜色允许的最大改变量（全范围的 0..1）。越低越安全保守；可防止模型产生生硬跳变。");
        changed |= SL("高光保护起点", &c.p.hiProtect, 0.5f, 1.0f, c.p.hiProtect >= 0.999f ? "关" : "%.2f");
        Tip("像素亮度从该水平升向白色时，模型的修改逐渐淡出，高光因此不被压碎。拉到最右（关）时修改作用于所有亮度。");
    }
    if (!kScalerAddon && eam::ui::SectionHeader("画面（锐化、色调、颜色、颗粒）")) {
        // Compose side, applied to every presented frame, real and generated alike.
        changed |= SL("锐化", &c.p.sharpen, 0.0f, 1.0f, c.p.sharpen <= 0.001f ? "关" : "%.2f");
        Tip("对每幅呈现帧的对比度自适应锐化，在模型修改叠加之后进行。模型和上采样都会让画面变柔；少量锐化（0.2 到 0.4）能找回力度。开销几乎为零。");
        changed |= SL("饱和度", &c.p.saturation, 0.0f, 2.0f, fabsf(c.p.saturation - 1.0f) < 0.005f ? "不变" : "%.2f");
        Tip("成品画面的颜色强度。1.00 = 不变；0 = 黑白；大于 1 = 更鲜艳。最后应用，真实帧与生成帧一视同仁。开销测不出来。");
        changed |= SL("自然饱和度", &c.p.vibrance, 0.0f, 1.0f, c.p.vibrance <= 0.001f ? "关" : "%.2f");
        Tip("类似饱和度，但对黯淡颜色的提升远大于鲜艳颜色，肤色和本就浓郁的颜色不会被继续推高。想要温和自然的增色用它；想要生硬的增色用饱和度。");
        changed |= SL("亮度", &c.p.brightness, -0.3f, 0.3f, fabsf(c.p.brightness) < 0.0005f ? "不变" : "%+.2f");
        Tip("所有像素加减同一数值（按画面自身 0 到 1 的刻度）。简单，但也会提亮黑色，可能让画面发灰：想要不发灰的提亮，先试 Gamma。");
        changed |= SL("对比度", &c.p.contrast, 0.5f, 1.5f, fabsf(c.p.contrast - 1.0f) < 0.002f ? "不变" : "%.2f");
        Tip("让画面远离中间灰（大于 1）或靠近它（小于 1）。调高时黑色更黑、白色更白；两端的细节可能被裁掉。");
        changed |= SL("Gamma", &c.p.gamma, 0.5f, 2.0f, fabsf(c.p.gamma - 1.0f) < 0.002f ? "不变" : "%.2f");
        Tip("弯曲中间调而不触及纯黑和纯白。大于 1 提亮中间调（展开昏暗场景）；小于 1 压暗。通常这是最好的亮度控制。");
        changed |= SL("阴影", &c.p.shadows, -1.0f, 1.0f, fabsf(c.p.shadows) < 0.005f ? "不变" : "%+.2f");
        Tip("只作用于画面暗部。大于 0 提亮（照亮暗角和地牢）；小于 0 加深。亮部保持不变。");
        changed |= SL("高光", &c.p.highlights, -1.0f, 1.0f, fabsf(c.p.highlights) < 0.005f ? "不变" : "%+.2f");
        Tip("只作用于画面亮部。小于 0 压低（找回天空和眩光）；大于 0 抬高。暗部保持不变。");
        changed |= SL("胶片颗粒", &c.p.grain, 0.0f, 1.0f, c.p.grain <= 0.002f ? "关" : "%.2f");
        Tip("细腻的单色噪点，中间调最强，每帧都不同。掩盖色带和上采样的塑料感。保持较低（0.1 到 0.3）。");
        { int gs = (int)c.p.grainSize; const int dgs = (int)kDefaults.grainSize; if (eam::ui::SliderInt("颗粒大小", &gs, 1, 4, "%d px", 0, &dgs)) { c.p.grainSize = (float)gs; changed = true; } }
        Tip("每个颗粒的大小，以屏幕像素计。1 最细；4K 屏幕上 2 最接近胶片。");
    }
    if (!kScalerAddon && eam::ui::SectionHeader("保持 HUD 原样")) {
        Note("画面保持与 Lossless Scaling 输出完全一致（无模型修改、锐化、色调、颜色或颗粒）的区域：用于动作条、小地图、聊天和文字。请在游戏画面的快照上绘制。");
        const bool busy = screenshot::Busy();
        if (busy) ImGui::BeginDisabled();
        if (eam::ui::Button(busy ? "正在截取..." : "截取快照", eam::ui::icons::kCheck, eam::ui::ButtonKind::Primary)) screenshot::RequestSnapshot();
        if (busy) ImGui::EndDisabled();
        Tip("游戏当前显示画面的一张快照，用于在其上绘制区域。游戏必须正在运行且正在缩放。随时可以再截一张。");
        ImGui::SameLine();
        { bool show = g_showHud; if (ImGui::Checkbox("在游戏中显示区域", &show)) g_showHud = show; }
        Tip("在游戏内把这些区域染成绿色并描边，方便对照你的 HUD。不会保存：调整完请关闭。");
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
        changed |= DrawHudEditor(c.p);
        Note("在画面上拖动即可添加区域（最多六个）。拖动区域可移动它，拖动边或角可调整大小。右键点击区域可删除。");
        changed |= SL("边缘柔化", &c.p.hudFeather, 0.0f, 0.05f, c.p.hudFeather <= 0.0005f ? "硬边缘" : "%.3f");
        Tip("区域之外增强效果渐入的平缓程度，以屏幕比例计。0 = 硬边缘。");
        if (ImGui::SmallButton("WoW 初始布局")) {
            const float d[4][4] = { { 0.0f, 0.0f, 0.30f, 0.17f }, { 0.86f, 0.0f, 1.0f, 0.24f }, { 0.0f, 0.70f, 0.26f, 1.0f }, { 0.27f, 0.88f, 0.73f, 1.0f } };
            memcpy(c.p.hud, d, sizeof d); c.p.hudCount = 4; changed = true;
        }
        Tip("对应《魔兽世界》默认界面位置的四个区域：头像框（左上）、小地图（右上）、聊天（左下）和动作条（下中）。作为在快照上调整的起点。");
        if (c.p.hudCount) { ImGui::SameLine(); if (ImGui::SmallButton("全部清除")) { c.p.hudCount = 0; changed = true; } }
        if (c.p.hudCount && ImGui::TreeNode("精确数值")) {
            for (uint32_t i = 0; i < c.p.hudCount; ++i) {
                ImGui::PushID(static_cast<int>(i));
                float* r = c.p.hud[i];
                ImGui::Text("区域 %u", i + 1);
                changed |= eam::ui::SliderFloat("左", &r[0], 0.0f, 0.99f); changed |= eam::ui::SliderFloat("上", &r[1], 0.0f, 0.99f);
                changed |= eam::ui::SliderFloat("右", &r[2], 0.01f, 1.0f); changed |= eam::ui::SliderFloat("下", &r[3], 0.01f, 1.0f);
                r[2] = std::max(r[2], r[0] + 0.01f); r[3] = std::max(r[3], r[1] + 0.01f);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        Note("区域随方案一起保存，每个游戏可以有自己的布局。");
    }
    if (kScalerAddon && eam::ui::SectionHeader("对比与快捷键")) {   // the upscalers: the upscaled picture or NIS's, and the sharpening
        int cm = g_compare.load() == 2 ? 1 : 0; const char* cms[] = { "上采样后", "Lossless Scaling 的 NIS（原画面）" };
        if (ImGui::Combo("对比视图", &cm, cms, 2)) g_compare = cm == 1 ? 2 : 0;
        Tip("上采样后 = 正常。Lossless Scaling 的 NIS = 如同插件关闭时的画面，用于对比（上采样器保持加载，切回即时生效）。不保存。");
        changed |= ImGui::Checkbox("快捷键：Ctrl+Shift + 按键（游戏获得焦点且上采样器运行时生效）", &c.hotkeys);
        Tip("在游戏内切换上采样器与 NIS、调整锐化。Ctrl+Shift 组合可避免与游戏自身按键绑定冲突。");
        auto fkey = [&](const char* label, int* vk) {
            int idx = *vk - VK_F1; if (idx < 0 || idx > 11) idx = 6;
            const char* names[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
            const ImGuiStyle& st = ImGui::GetStyle();
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("F12").x + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x);
            if (ImGui::Combo(label, &idx, names, 12)) { *vk = VK_F1 + idx; changed = true; }
        };
        fkey("前后对比", &c.keyAB); fkey("锐化 -", &c.keySharpDn); fkey("锐化 +", &c.keySharpUp);
        fkey("前后对比截图", &c.keyShot);
        auto fname = [](int vk) { static char b[4][8]; static int n = 0; char* o = b[n++ & 3]; snprintf(o, 8, "F%d", vk - VK_F1 + 1); return (const char*)o; };
        Note("当前：Ctrl+Shift+%s 前后对比  |  %s / %s 锐化 - / +  |  %s 前后对比截图  (%s)", fname(c.keyAB),
             fname(c.keySharpDn), fname(c.keySharpUp), fname(c.keyShot), c.hotkeys ? "快捷键已开启" : "快捷键已关闭：请勾选上方复选框");
        if (ImGui::SmallButton("保存前后对比截图")) RequestPair();
        Tip("同一时刻的两张 PNG：上采样画面与 Lossless Scaling 的 NIS（拍摄时会显示一帧 NIS）。"
            "Windows 自带截图截取的是游戏窗口，不是 Lossless Scaling 的画面。HDR 游戏按其 SDR 视图保存。");
        ImGui::SameLine();
        if (ImGui::SmallButton("打开截图文件夹")) ShellExecuteW(nullptr, L"open", screenshot::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        { bool ok; const std::string msg = screenshot::LastResult(ok);
          if (!msg.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("%s", msg.c_str()); ImGui::PopStyleColor(); } }
    }
    if (!kScalerAddon && eam::ui::SectionHeader("对比与快捷键")) {
        int cm = g_compare; const char* cms[] = { "增强后", "分割：左原画面 | 右增强后", "仅原画面（对比用）" };
        if (ImGui::Combo("对比视图", &cm, cms, 3)) g_compare = cm;
        Tip("增强后 = 正常。分割 = 线左侧为原画面，右侧为增强后。仅原画面 = 如同插件关闭（省去合成工作，但模型继续运行）。仅显示用；不保存。");
        float sp = g_splitPos; { const float dsp = 0.5f; if (eam::ui::SliderFloat("分割位置", &sp, 0.05f, 0.95f, "%.2f", 0, &dsp)) g_splitPos = sp; }
        Tip("分割线的位置，从屏幕左缘（0）到右缘（1）。");
        Note("仅显示用：模型继续运行，切换即时生效。不保存：Lossless Scaling 总是以增强后启动。");
        changed |= ImGui::Checkbox("快捷键：Ctrl+Shift + 按键（游戏获得焦点时生效）", &c.hotkeys);
        Tip("在游戏内切换对比视图、锐化和预设。它们在 Lossless Scaling 呈现帧时被读取。Ctrl+Shift 组合可避免与游戏自身按键绑定冲突。");
        auto fkey = [&](const char* label, int* vk) {
            int idx = *vk - VK_F1; if (idx < 0 || idx > 11) idx = 6;
            const char* names[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
            // wide enough for "F12" plus the arrow at any display scale (a fixed pixel width clipped the key name)
            const ImGuiStyle& st = ImGui::GetStyle();
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("F12").x + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x);
            if (ImGui::Combo(label, &idx, names, 12)) { *vk = VK_F1 + idx; changed = true; }
        };
        fkey("前后对比", &c.keyAB); fkey("分割视图", &c.keySplit); fkey("锐化 -", &c.keySharpDn); fkey("锐化 +", &c.keySharpUp); fkey("下一个预设", &c.keyPreset);
        fkey("截图", &c.keyShot);
        auto fname = [](int vk) { static char b[8][8]; static int n = 0; char* o = b[n++ & 7]; snprintf(o, 8, "F%d", vk - VK_F1 + 1); return (const char*)o; };
        Note("当前：Ctrl+Shift+%s 前后对比  |  %s 分割  |  %s / %s 锐化 - / +  |  %s 下一个预设  |  %s 截图  (%s)",
            fname(c.keyAB), fname(c.keySplit), fname(c.keySharpDn), fname(c.keySharpUp), fname(c.keyPreset), fname(c.keyShot),
            c.hotkeys ? "快捷键已开启" : "快捷键已关闭：请勾选上方复选框");
        Note("屏幕左上角会短暂出现一个小方块：绿色为增强后，红色为原画面，琥珀色为分割，蓝色为锐化已更改，紫色为预设。");
    }
    if (!kScalerAddon && eam::ui::SectionHeader("截图")) {
        Note("按你所见保存画面（包含插件的效果、缩放和帧生成）为 PNG。它在 Lossless Scaling 显示的下一帧拍摄，所以游戏必须正在运行且正在缩放。");
        const bool busy = screenshot::Busy();
        if (busy) ImGui::BeginDisabled();
        if (eam::ui::Button(busy ? "正在截图..." : "截图", eam::ui::icons::kCheck, eam::ui::ButtonKind::Primary)) screenshot::Request();
        if (busy) ImGui::EndDisabled();
        Tip("所见画面，一张 PNG。");
        ImGui::SameLine();
        if (eam::ui::Button("保存前后对比截图", eam::ui::icons::kCheck)) RequestPair();
        Tip("同一时刻的两张 PNG：\"_NR\" 带插件效果，\"_original\" 不带（拍摄时会显示一帧原画面）。"
            "在游戏中按 Ctrl+Shift + 截图键（见对比与快捷键一节）亦可。HDR 游戏按其 SDR 视图保存。");
        const std::wstring folder = screenshot::Folder();
        const int size = WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string folderText(size > 0 ? size - 1 : 0, '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, folderText.data(), size, nullptr, nullptr);
        ImGui::TextWrapped("文件夹：%s", folderText.c_str());
        const bool choosing = screenshot::Choosing();
        if (choosing) ImGui::BeginDisabled();
        if (ImGui::SmallButton(choosing ? "正在等待文件夹对话框..." : "选择文件夹...")) screenshot::ChooseFolder();
        if (choosing) ImGui::EndDisabled();
        Tip("截图的存放位置。第一次是“图片”文件夹下名为 Lossless Scaling 的文件夹。");
        ImGui::SameLine();
        if (ImGui::SmallButton("打开文件夹")) { CreateDirectoryW(folder.c_str(), nullptr); ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
        if (!c.screenshotFolder.empty()) { ImGui::SameLine(); if (ImGui::SmallButton("改用“图片”文件夹")) { c.screenshotFolder.clear(); changed = true; } }
        bool ok; const std::string result = screenshot::LastResult(ok);
        if (!result.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("%s", result.c_str()); ImGui::PopStyleColor(); }
    }
    if (!kScalerAddon && eam::ui::SectionHeader("游戏（每个程序一套方案）")) {
        std::string cur; { std::lock_guard<std::mutex> lk(g_textMutex); cur = g_focusExe; }
        ImGui::Text("焦点程序：%s", cur.empty() ? "（尚未见到程序）" : cur.c_str());
        Tip("最近获得焦点的程序，忽略 Lossless Scaling 自己的窗口。游戏正在缩放时即为该游戏。");
        changed |= ImGui::Checkbox("程序获得焦点时切换到其保存的方案", &c.gameAuto);
        Tip("下方列出的程序获得焦点时，自动应用其方案（每次切换程序只应用一次，之后你自己的微调会保留）。方案的模型分辨率不同会让模型重建片刻。");
        std::vector<std::string> names; { std::lock_guard<std::mutex> lk(g_settingsMutex); for (auto& pr : g_looks) names.push_back(pr.name); }
        int rmg = -1;
        for (size_t i = 0; i < c.games.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(c.games[i].first.c_str()); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            if (ImGui::BeginCombo("##gp", c.games[i].second.c_str())) {
                for (auto& n : names) if (ImGui::Selectable(n.c_str(), n == c.games[i].second)) { c.games[i].second = n; changed = true; }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); if (ImGui::SmallButton("清除")) rmg = (int)i;
            ImGui::PopID();
        }
        if (rmg >= 0) { ForgetGame(g_host, kAddonId, c.games[rmg].first); c.games.erase(c.games.begin() + rmg); changed = true; }
        if (c.games.empty()) ImGui::TextDisabled("还没有程序。");
        static char exeBuf[64] = ""; static int addSel = 0;
        ImGui::InputText("程序（exe 名称）", exeBuf, sizeof exeBuf);
        Tip("例如 WowB.exe。匹配不区分大小写。");
        ImGui::SameLine(); if (ImGui::SmallButton("使用焦点程序") && !cur.empty()) snprintf(exeBuf, sizeof exeBuf, "%s", cur.c_str());
        if (addSel >= (int)names.size()) addSel = 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::BeginCombo("使用的方案", names.empty() ? "（请先保存一个方案）" : names[addSel].c_str())) {
            for (int i = 0; i < (int)names.size(); ++i) if (ImGui::Selectable(names[i].c_str(), i == addSel)) addSel = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("添加") && exeBuf[0] && !names.empty()) {
            std::string e = exeBuf; for (char& ch : e) ch = (char)tolower((unsigned char)ch);
            e = CleanName(e); bool found = false;
            for (auto& g : c.games) if (g.first == e) { g.second = names[addSel]; found = true; }
            if (!found && !e.empty()) c.games.push_back({ e, names[addSel] });
            changed = true;
        }
        Tip("该程序每次获得焦点时，使用所选的保存方案。");
        if (ImGui::Button("为焦点程序保存当前方案") && !cur.empty()) {
            std::string base = cur; const size_t dot = base.rfind('.'); if (dot != std::string::npos && dot > 0) base.resize(dot);
            const std::string n = CleanName(base); const std::string d = LookToText(c.p);
            { std::lock_guard<std::mutex> lk(g_settingsMutex); bool found = false; for (auto& pr : g_looks) if (pr.name == n) { pr.data = d; found = true; } if (!found) g_looks.push_back({ n, d }); }
            bool found = false; for (auto& g : c.games) if (g.first == cur) { g.second = n; found = true; }
            if (!found) c.games.push_back({ cur, n });
            changed = true;
        }
        Tip("把你现在设置的全部内容（包括 HUD 区域）存为以该程序命名的方案，并把该程序与它关联。");
    }
    if (eam::ui::SectionHeader("帧检测（高级）")) {
        Note("插件如何识别 Lossless Scaling 的新真实帧。自动通常可用；仅当状态一直卡在等待时才修改此项。");
        const int modeBefore = c.tapMode;
        if (ImGui::RadioButton("自动", c.tapMode == 0)) c.tapMode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("手动", c.tapMode == 1)) c.tapMode = 1;
        tapChanged |= c.tapMode != modeBefore;
        const char* slotNames[] = { "自动（最高）", "S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7" };
        int slot = c.frameSlot + 1;
        if (ImGui::Combo("帧槽位", &slot, slotNames, 9)) { c.frameSlot = slot - 1; tapChanged = true; }
        DispatchSig tick, tap;
        g_tap.GetRoles(tick, tap);
        auto role = [](const char* name, const DispatchSig& sig) {
            ImGui::TextWrapped("%s %s (%u,%u,%u) %s", name, sig.Empty() ? "-" : "", sig.x, sig.y, sig.z, PassText(sig).c_str());
        };
        role("TICK:", tick);
        role("TAP: ", tap);
        if (ImGui::SmallButton("清空表格")) g_tap.ClearTable();
        ImGui::SameLine();
        if (ImGui::SmallButton("清空角色")) { c.tickSig.clear(); c.tapSig.clear(); tapChanged = true; }
        // the passes seen, busiest first; either can be made the TAP or the TICK by hand
        std::vector<DispatchEntry> passes = g_tap.Snapshot();
        std::sort(passes.begin(), passes.end(), [](const DispatchEntry& a, const DispatchEntry& b) { return a.count > b.count; });
        if (passes.size() > 24) passes.resize(24);
        if (ImGui::BeginTable("passes", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const char* column : { "次数", "组", "视图", "自动", "指派" }) ImGui::TableSetupColumn(column);
            ImGui::TableHeadersRow();
            for (const DispatchEntry& pass : passes) {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(pass.key & 0x7fffffff));
                ImGui::TableNextColumn(); ImGui::Text("%u", pass.count);
                ImGui::TableNextColumn(); ImGui::Text("%u,%u,%u", pass.sig.x, pass.sig.y, pass.sig.z);
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", PassText(pass.sig).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(pass.roleAuto == 1 ? "TICK" : pass.roleAuto == 2 ? "TAP" : "");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("TAP")) { c.tapSig = pass.sig.Serialize(); c.tapMode = 1; tapChanged = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("TICK")) { c.tickSig = pass.sig.Serialize(); c.tapMode = 1; tapChanged = true; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    if (eam::ui::SectionHeader("技术状态")) {
    ImGui::Text("上一个 LS 设备：%s %s   引擎：%s", adapterName.c_str(), hasDisplay ? "（驱动显示器）" : "（无显示输出）",
                kScalerAddon ? ScalerEngineText().c_str() : g_engineCardKnown ? "在 LSFG 设备上" : "未启动");
    ImGui::Text("帧：%s   NR %.1f ms（平均 %.1f）  本次 %.1f ms   运行 %llu 次   失败 %llu 次", frameInfo.c_str(), g_lastModelMs, g_avgModelMs, g_lastRunMs, (unsigned long long)g_runs, (unsigned long long)g_engine.Stats().fails);
    ImGui::Text("分发 %llu  TICK %llu  TAP %llu  门控：%s  浮点槽 %d", (unsigned long long)g_tap.Dispatches(), (unsigned long long)g_tap.Ticks(), (unsigned long long)g_tap.Taps(), g_tap.GateName(), g_engine.Stats().floatSlot);
    { std::string tdi; { std::lock_guard<std::mutex> lk(g_textMutex); tdi = g_tappedDeviceText; }
      ImGui::Text("已见处理趟：%llu   其他适配器处理趟：%llu   截取设备：%s", (unsigned long long)g_tap.Dispatches(), (unsigned long long)g_otherPasses, tdi.c_str()); }
    }
    if (eam::ui::SectionHeader("高级")) {
        int dv = (int)c.p.debugView; const char* views[] = { "结果", "原画面", "增量 x4", "帧角色（绿=真实帧，红=生成帧）", "LSFG 光流", "残影防护（白色=完全生效）" };
        if (ImGui::Combo("诊断视图", &dv, views, 6)) { c.p.debugView = dv; changed = true; }
        Tip("显示插件正在做什么而非成品画面：原帧、放大 4 倍的模型修改、哪些帧是真实或生成的、或运动数据。正常使用请留在“结果”。");
        changed |= SL("慢模型看门狗（ms）", &c.watchdogMs, 20.0f, 200.0f, "%.0f");
        Tip("若模型连续 30 帧超过此时长，插件会自行关闭，绝不会拖累你的帧率。10 秒后自动重新启用，每次会话最多三次。");
        char sp[512]; strncpy(sp, c.snippetPath.c_str(), sizeof sp); sp[sizeof sp - 1] = 0;
        if (ImGui::InputText("模型文件路径（留空 = Lossless Scaling 文件夹）", sp, sizeof sp)) { c.snippetPath = sp; changed = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { { std::lock_guard<std::mutex> lk(g_settingsMutex); g_config.snippetPath = c.snippetPath; } ScanRequirements(); }
        Tip("nvngx_dlssnr.dll 的完整路径。留空则使用 LosslessScaling.exe 旁边的副本。");
        if (ImGui::SmallButton("重启引擎")) { SwitchOn(); RestartEngine(); }
        Tip("拆掉模型并在 Lossless Scaling 正在使用的显卡上重新启动。");
        ImGui::SameLine();
        if (ImGui::SmallButton("导出光流探测（3 帧）")) g_tap.ArmProbe(g_lsDir);
        Tip("诊断用：把接下来三帧和 Lossless Scaling 的运动数据写入其文件夹中的文件。");
        { std::string ps = g_tap.ProbeStatus(); if (ps != "idle") ImGui::TextWrapped("%s", ps.c_str()); }
        Note("present 挂钩：%s，进程中已见 %u 次呈现   日志：<LS 文件夹>\\logs\\DLSS5NR01.log", PresentHook::Installed() ? "已安装" : "尚未安装", PresentHook::Hits());
    }

    if (changed || createChanged || tapChanged) Commit(c, tapChanged, createChanged);
    if (modelChanged) RestartEngine();   // the DLAA model is chosen when the engine starts
}

} // namespace nr
