#include "../widgets/update_offer.h"
#include "tab_about.h"
#include "../gui_scale.h"
#include "../gui_style.h"
#include "../widgets/status_bar.h"
#include "../../update/update_check.h"
#include "../../../sdk/include/eam/version.h"
#include "imgui.h"
#include "eam/widgets.h"
#include <cstdio>
#include <windows.h>
#include <shellapi.h>

namespace eam {

static void OpenUrl(const char* url) { ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL); }

static void CenteredText(const char* text, bool muted = false) {
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f + ImGui::GetCursorPosX());
    if (muted) ImGui::TextDisabled("%s", text); else ImGui::TextUnformatted(text);
}

void RenderTabAbout() {
    ImGui::Dummy(ImVec2(0, S(12)));

    // logo: the Echo mark on a dark tile, drawn as vectors (crisp at any scale)
    {
        const float s = ImGui::GetFontSize() * 4.2f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - s) * 0.5f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), eam::ui::theme::U(eam::ui::theme::kPanel), s * 0.22f);
        dl->AddRect(p, ImVec2(p.x + s, p.y + s), eam::ui::theme::U(eam::ui::theme::kAccentDim), s * 0.22f, 0, 1.5f);
        const float m = s * 0.06f;
        const float in = s - 2 * m;
        eam::ui::svg::Draw(dl, eam::ui::icons::kEchoBack, ImVec2(p.x + m, p.y + m), in, eam::ui::theme::U(eam::ui::theme::kAccentDim), 1.8f);
        eam::ui::svg::Draw(dl, eam::ui::icons::kEchoMid, ImVec2(p.x + m, p.y + m), in, eam::ui::theme::U(eam::ui::theme::kAccent), 1.8f);
        eam::ui::svg::Draw(dl, eam::ui::icons::kEchoFront, ImVec2(p.x + m, p.y + m), in, eam::ui::theme::U(eam::ui::theme::kAccentHot), 1.8f, eam::ui::theme::U(eam::ui::theme::kAccent));
        ImGui::Dummy(ImVec2(s, s));
        ImGui::Dummy(ImVec2(0, S(6)));
    }

    // title block
    CenteredText(EAM_PRODUCT_NAME);
    char versionStr[64];
    snprintf(versionStr, sizeof(versionStr), "v%s", EAM_VERSION_STRING);
    CenteredText(versionStr, true);
    {   // is there a newer one? (the daily check, or the button)
        const update::Status st = update::Current();
        const std::string line = update::DescribeStatus(st);
        if (st.state == update::State::Available) {
            ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kAccent));
            CenteredText(line.c_str());
            ImGui::PopStyleColor();
            const float bw1 = ImGui::CalcTextSize("下载并安装").x + ImGui::GetFontSize() * 4.0f;
            const float bw2 = ImGui::CalcTextSize("打开下载页面").x + ImGui::GetFontSize() * 4.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - bw1 - bw2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
            if (eam::ui::Button("下载并安装", eam::ui::icons::kDownload, eam::ui::ButtonKind::Primary, ImVec2(bw1, 0))) widgets::OpenUpdateOffer();
            ImGui::SameLine();
            if (eam::ui::Button("打开下载页面", eam::ui::icons::kExternal, eam::ui::ButtonKind::Flat, ImVec2(bw2, 0))) OpenUrl(st.url.c_str());
        } else {
            if (st.state != update::State::Idle) CenteredText(line.c_str(), true);
            const bool checking = st.state == update::State::Checking;
            const float bw2 = ImGui::CalcTextSize("检查更新").x + ImGui::GetFontSize() * 3.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - bw2) * 0.5f);
            if (checking) ImGui::BeginDisabled();
            if (eam::ui::Button("检查更新", eam::ui::icons::kCheck, eam::ui::ButtonKind::Flat, ImVec2(bw2, 0))) update::StartCheckAsync();
            if (checking) ImGui::EndDisabled();
        }
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    CenteredText("Lossless Scaling 增强版：神经渲染、DLSS 与 FSR 上采样，以及插件带来的更多可能。", true);

    // support
    ImGui::Dummy(ImVec2(0, S(16)));
    const float bw = ImGui::CalcTextSize("在 Ko-fi 上支持").x + ImGui::GetFontSize() * 4.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - bw) * 0.5f);
    if (eam::ui::Button("在 Ko-fi 上支持", eam::ui::icons::kHeart, eam::ui::ButtonKind::Primary, ImVec2(bw, ImGui::GetFrameHeight() * 1.25f)))
        widgets::OpenKofi();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("如果它对你有帮助，一杯 Ko-fi 就是对我们很大的支持。会打开浏览器。");

    // what comes with it: the plugins and what is built into the manager
    ImGui::Dummy(ImVec2(0, S(18)));
    eam::ui::SectionLabel("内置内容");
    {
        struct Item { const char* icon; const char* name; const char* what; };
        const Item items[] = {
            { eam::ui::icons::kSparkles, "DLSS 5 神经渲染",
              "NVIDIA 的神经渲染模型作用于 Lossless Scaling 显示的每一帧，无论真实帧还是生成帧：为任意游戏带来全新画面风格，"
              "自带独立的运动测量、可保存的风格和按游戏的风格。需要 NVIDIA RTX；模型文件需自备。" },
            { eam::ui::icons::kUpscale, "DLSS 上采样器",
              "用 NVIDIA DLSS 取代 Lossless Scaling 的 NIS 缩放器：为从未支持过 DLSS 的游戏带来真正的上采样"
              "（或在屏幕原始分辨率下使用 DLAA），运动信息从画面帧中测量得到。需要 NVIDIA RTX。" },
            { eam::ui::icons::kUpscaleFast, "FSR 上采样器",
              "AMD FSR（3.1 或 FSR 4）也可用于任意 DirectX 12 显卡。两个上采样器同样支持 4:3 游戏、按游戏保存设置，"
              "并为没有抗锯齿的老游戏提供“稳定性”和“边缘平滑”。" },
            { eam::ui::icons::kUpscale, "视频超分辨率",
              "用 NVIDIA 的 RTX Video Super Resolution 取代 NIS，作用于电影、图片、文字和界面：在时序上采样器只能保持平滑之处呈现锐利。"
              "未变化的画面不会被重复处理。需要 NVIDIA RTX；所需的 NVIDIA 文件已随附。" },
            { eam::ui::icons::kSliders, "管理器内置",
              "ReShade 输入透传、窗口化游戏与第二显示器、指出帧率瓶颈所在的性能页、托盘图标和快捷键、"
              "可在 Lossless Scaling 更新后自我修复的单文件安装器，以及更新检查。" },
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float icon = S(30.0f);
        for (const Item& it : items) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(at, ImVec2(at.x + icon, at.y + icon), eam::ui::theme::U(eam::ui::theme::kPanelAlt), S(4.0f));
            dl->AddRect(at, ImVec2(at.x + icon, at.y + icon), eam::ui::theme::U(eam::ui::theme::kAccentDim), S(4.0f));
            eam::ui::svg::Draw(dl, it.icon, ImVec2(at.x + icon * 0.19f, at.y + icon * 0.19f), icon * 0.62f, eam::ui::theme::U(eam::ui::theme::kAccent), 1.7f);
            ImGui::Dummy(ImVec2(icon, icon));
            ImGui::SameLine(0, S(12));
            ImGui::BeginGroup();
            if (ImFont* title = TitleFont()) ImGui::PushFont(title, 0.0f);
            ImGui::TextUnformatted(it.name);
            if (TitleFont()) ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("%s", it.what);
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, S(6)));
        }
    }

    ImGui::Dummy(ImVec2(0, S(14)));
    eam::ui::SectionLabel("构建信息");
    ImGui::TextDisabled("插件 API");   ImGui::SameLine(S(120)); ImGui::Text("v%s", EAM_API_VERSION_STRING);
    ImGui::TextDisabled("ImGui");        ImGui::SameLine(S(120)); ImGui::Text("%s", ImGui::GetVersion());
    ImGui::TextDisabled("构建时间");   ImGui::SameLine(S(120)); ImGui::Text("%s %s", __DATE__, __TIME__);

    ImGui::Dummy(ImVec2(0, S(14)));
    eam::ui::SectionLabel("实用提示");
    ImGui::TextWrapped("关掉了这个窗口？它仍在通知区域（托盘）中运行。点击托盘图标，或使用“设置”中配置的快捷键，"
                       "即可再次打开。");
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::TextWrapped("按住 Ctrl 并在滑块上滚动滚轮可微调数值；双击滑块可恢复默认值。");

    ImGui::Dummy(ImVec2(0, S(14)));
    eam::ui::SectionLabel("致谢");
    ImGui::TextWrapped("本项目源自 FrankBarretta 的 LosslessProxy（MIT），我们对此心怀感激：用代理 Lossless.dll 加载插件的思路、"
                       "插件接口（这也是 LosslessProxy 的插件至今仍能在这里加载的原因），以及 ReShade 和窗口化功能——"
                       "它们最初是该项目的插件，如今已内置。此后的代码已被完全重写。");
    ImGui::Dummy(ImVec2(0, S(4)));
    if (eam::ui::Button("原项目", eam::ui::icons::kExternal, eam::ui::ButtonKind::Flat)) OpenUrl("https://github.com/FrankBarretta/LosslessProxy");
    ImGui::Dummy(ImVec2(0, S(10)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("NVIDIA DLSS：神经渲染插件使用 NVIDIA DLSS 技术，并依据 NVIDIA RTX SDK 许可证（其文件夹中的 NVIDIA-LICENSE.txt）"
                       "包含 NVIDIA 的 NGX SDK 和 DLSS 运行时。NVIDIA、NVIDIA 标志和 DLSS 是 NVIDIA "
                       "Corporation 的商标。本项目与 NVIDIA 无关联，也未获得其认可。");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("AMD FidelityFX：FSR 上采样器包含 AMD 的 FidelityFX 运行时（MIT 许可证，其文件夹中的 AMD-FidelityFX-LICENSE.txt）。AMD、"
                       "FidelityFX 和 FSR 是 Advanced Micro Devices, Inc. 的商标；本项目与 AMD 无关联，也未获得其认可。");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::TextDisabled("此外：Dear ImGui 和 nlohmann/json（MIT）、stb_image（公共领域）、MinHook（BSD，用于窗口化模式）。");
    ImGui::TextDisabled("图标形状按 Lucide 图标集的风格绘制（ISC）。");
    ImGui::TextDisabled("Lossless Scaling 归其作者所有；这是它的非官方插件。");

    ImGui::Dummy(ImVec2(0, S(14)));
    eam::ui::SectionLabel("给插件作者");
    ImGui::TextDisabled("包含 SDK 头文件（addon_sdk.h）即可。widgets.h 让插件拥有与本管理器相同的外观：调色板、");
    ImGui::TextDisabled("带默认值的滑块、分区标题、按钮和 SVG 图标。");
}

} // namespace eam
