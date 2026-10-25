#include "header_bar.h"
#include "tooltip.h"
#include "../gui_scale.h"
#include "../gui_style.h"
#include "update_offer.h"
#include "../../update/update_check.h"
#include "../../host/gpu_stats.h"
#include "../../host/system_stats.h"
#include "eam/icons.h"
#include "eam/version.h"
#include "eam/widgets.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace eam {
namespace widgets {

namespace {

using namespace eam::ui::theme;

// A memory figure's colour by how full it is: green, amber past 80%, red past 93%.
const float* FullnessColour(double used, double total) {
    const double f = total > 0 ? used / total : 0;
    return f < 0.80 ? kAccent : f < 0.93 ? kWarn : kDanger;
}
// A load figure's colour: plain text, amber when pinned (a game at its limit is normal; this only says there is no room left).
const float* LoadColour(double percent) { return percent < 95.0 ? kText : kWarn; }

struct Reading { const char* caption; std::string value; const float* colour; };

std::string Gigabytes(uint64_t usedMB, uint64_t totalMB) {
    char text[48];
    snprintf(text, sizeof text, "%.1f / %.0f GB", usedMB / 1024.0, totalMB / 1024.0);
    return text;
}

// The logo: the three stacked tiles of the About tab, small.
void Logo(ImDrawList* dl, ImVec2 at, float size) {
    eam::ui::svg::Draw(dl, eam::ui::icons::kEchoBack, at, size, U(kAccentDim), 1.8f);
    eam::ui::svg::Draw(dl, eam::ui::icons::kEchoMid, at, size, U(kAccent), 1.8f);
    eam::ui::svg::Draw(dl, eam::ui::icons::kEchoFront, at, size, U(kAccentHot), 1.8f, U(kAccent));
}

// The scaling cheat-sheet behind the header bar's ? button: one row per scheme Lossless Scaling offers,
// in its own list's order (the popup draws the NIS row's advice cell in the accent colour).
constexpr const char* kScalingHelpPopup = "LS 缩放方案速查###ls_scaling_help";
struct ScalingRow { const char* scheme; const char* what; const char* suits; };
const ScalingRow kScalingRows[] = {
    { "NIS",               "NVIDIA 图像缩放：空间上采样 + 锐化",   "3D 游戏压分辨率后放大；插件链路的唯一入口" },
    { "FSR 1",             "同类空间上采样（AMD 版）+ 锐化",      "不开插件时给 3D 游戏放大，与 NIS 按口味二选一" },
    { "Nearest（最邻近）",  "一像素对一像素硬放大，零模糊",        "像素画游戏，且缩放比正好整数倍（1080p 到 2160p）" },
    { "Sharp Bilinear",    "先整数倍最邻近，再双线性补零头",      "像素画但缩放比非整数（1080p 到 1440p）" },
    { "Bilinear",          "双线性平滑，无锐化",                  "视频、已抗锯齿好的画面" },
    { "Anime4K",           "面向动画的卷积网络恢复 + 放大",        "动漫风 2D 游戏、动画视频" },
    { "Auto",              "Lossless Scaling 自动判断内容挑算法", "懒人默认；要精确控制就手动选" },
};

// A cheat-sheet cell: plain text, wrapped at the caller's wrap position (the NIS row's advice cell
// is coloured by the caller, see the popup in HeaderBar).
void CheatCell(const char* text) { ImGui::TextUnformatted(text); }

// A warning line: the kAlert mark, then the text in the theme's amber. Wraps at the caller's wrap position.
void WarnLine(const char* text) {
    const float ico = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    eam::ui::svg::Draw(ImGui::GetWindowDrawList(), eam::ui::icons::kAlert, p, ico, U(kWarn), 2.0f);
    ImGui::Dummy(ImVec2(ico + ImGui::GetStyle().ItemInnerSpacing.x, ico));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, V(kWarn));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

} // namespace

float HeaderBarHeight() { return S(50.0f); }

void HeaderBar() {
    // EAM_PREVIEW_SHEET=1 (the offline preview, tools/ui_preview.cpp): open the cheat-sheet until it
    // is up, so a picture can be taken of it — the ? button is not clickable there. OpenPopup only
    // marks it for the current window; it is retried every frame until BeginPopupModal takes it.
    static const bool s_sheetPreview = [] { char* v = nullptr; size_t n = 0; _dupenv_s(&v, &n, "EAM_PREVIEW_SHEET"); const bool on = v != nullptr; free(v); return on; }();
    if (s_sheetPreview && !ImGui::IsPopupOpen(kScalingHelpPopup)) ImGui::OpenPopup(kScalingHelpPopup);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = HeaderBarHeight();
    ImFont* title = TitleFont();
    const float base = ImGui::GetStyle().FontSizeBase;

    // left: logo, name, version
    const float logo = S(28.0f);
    Logo(dl, ImVec2(origin.x, origin.y + (height - logo) * 0.5f - S(2)), logo);
    float x = origin.x + logo + S(12);
    const float titleSize = base * 1.25f * ImGui::GetStyle().FontScaleDpi;
    const float titleY = origin.y + (height - titleSize) * 0.5f - S(2);
    dl->AddText(title ? title : ImGui::GetFont(), titleSize, ImVec2(x, titleY), U(kText), "插件管理器");
    x += (title ? title : ImGui::GetFont())->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "插件管理器").x + S(8);
    const float smallSize = ImGui::GetFontSize() * 0.92f;
    const float smallY = titleY + titleSize - smallSize - S(1);
    dl->AddText(ImGui::GetFont(), smallSize, ImVec2(x, smallY), U(kMuted), "适用于 Lossless Scaling");
    x += ImGui::GetFont()->CalcTextSizeA(smallSize, FLT_MAX, 0.0f, "适用于 Lossless Scaling").x + S(10);
    const std::string version = std::string("v") + EAM_VERSION_STRING;
    const ImVec2 vs = ImGui::GetFont()->CalcTextSizeA(smallSize * 0.92f, FLT_MAX, 0.0f, version.c_str());
    const ImVec2 chip(x, smallY - S(1));
    dl->AddRect(chip, ImVec2(chip.x + vs.x + S(10), chip.y + vs.y + S(3)), U(kAccentDim), S(3.0f));
    dl->AddText(ImGui::GetFont(), smallSize * 0.92f, ImVec2(chip.x + S(5), chip.y + S(1.5f)), U(kAccent), version.c_str());
    float leftEnd = chip.x + vs.x + S(10);
    {   // an update is waiting (also after "Not now" or "Don't ask again"): a badge that opens the offer
        const update::Status us = update::Current();
        if (us.state == update::State::Available && !us.latest.empty()) {
            const std::string label = "有新版本 " + us.latest + " 可更新";
            const float fs = smallSize * 0.92f;
            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, label.c_str());
            const ImVec2 p0(leftEnd + S(10), chip.y - S(1)), p1(p0.x + ts.x + S(14), p0.y + ts.y + S(5));
            const ImVec2 saved = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(p0);
            const bool clicked = ImGui::InvisibleButton("##update_badge", ImVec2(p1.x - p0.x, p1.y - p0.y));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::SetCursorScreenPos(saved);
            dl->AddRectFilled(p0, p1, hovered ? U(kAccent) : U(kAccentDim), S(3.0f));
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p0.x + S(7), p0.y + S(2.5f)), hovered ? U(kBg) : U(kText), label.c_str());
            if (hovered) Tip("有新版本可用。点击查看更新内容并安装。");
            if (clicked) OpenUpdateOffer();
            leftEnd = p1.x;
        }
    }

    // right: the machine, right to left
    GpuStats::Instance().Wanted();   // keeps the card's sampler running while the window is drawn
    const GpuStats::Snapshot g = GpuStats::Instance().Get();
    const SystemStats::Snapshot s = SystemStats::Instance().Get();
    std::vector<Reading> readings;
    if (g.ok) {
        char load[16]; snprintf(load, sizeof load, "%u%%", g.utilGpu);
        readings.push_back({ "GPU", load, LoadColour(g.utilGpu) });
        if (g.vramTotalMB) readings.push_back({ "VRAM", Gigabytes(g.vramUsedMB, g.vramTotalMB), FullnessColour((double)g.vramUsedMB, (double)g.vramTotalMB) });
    }
    if (s.ok) {
        readings.push_back({ "RAM", Gigabytes(s.ramUsedMB, s.ramTotalMB), FullnessColour((double)s.ramUsedMB, (double)s.ramTotalMB) });
        char cpu[16]; snprintf(cpu, sizeof cpu, "%.0f%%", s.cpuPercent);
        readings.push_back({ "CPU", cpu, LoadColour(s.cpuPercent) });
    }
    const float captionSize = ImGui::GetFontSize() * 0.78f, valueSize = ImGui::GetFontSize() * 1.02f;
    const float gap = S(22);
    float right = origin.x + width;
    float readoutLeft = right;
    for (size_t i = readings.size(); i-- > 0;) {
        const Reading& r = readings[i];
        ImFont* vf = title ? title : ImGui::GetFont();
        const float vw = vf->CalcTextSizeA(valueSize, FLT_MAX, 0.0f, r.value.c_str()).x;
        const float cw = ImGui::GetFont()->CalcTextSizeA(captionSize, FLT_MAX, 0.0f, r.caption).x;
        const float w = vw > cw ? vw : cw;
        const float left = right - w;
        if (left < leftEnd + S(20)) break;   // a narrow window: the readings that do not fit are left out
        const float top = origin.y + (height - (captionSize + valueSize + S(2))) * 0.5f - S(2);
        dl->AddText(ImGui::GetFont(), captionSize, ImVec2(right - cw, top), U(kMuted), r.caption);
        dl->AddText(vf, valueSize, ImVec2(right - vw, top + captionSize + S(2)), U(r.colour), r.value.c_str());
        readoutLeft = left;
        right = left - gap;
        if (i > 0) dl->AddLine(ImVec2(right + gap * 0.5f, top + S(2)), ImVec2(right + gap * 0.5f, top + captionSize + valueSize), U(kBorderBright), 1.0f);
    }

    {   // the scaling cheat-sheet: a small ? after the readouts (the band lays its right side out right to left, so after them means to their left)
        const float bs = S(26);
        const ImVec2 bp(readoutLeft - S(16) - bs, origin.y + (height - bs) * 0.5f - S(2));
        if (bp.x >= leftEnd + S(12)) {   // a narrow window: no button rather than one over the title or the update badge
            const ImVec2 saved = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(bp);
            const bool clicked = ImGui::InvisibleButton("##scaling_help", ImVec2(bs, bs));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::SetCursorScreenPos(saved);
            if (hovered) dl->AddRectFilled(bp, ImVec2(bp.x + bs, bp.y + bs), U(kRowHover), S(3.0f));
            const ImVec2 qs = ImGui::CalcTextSize("?");
            dl->AddText(ImVec2(bp.x + (bs - qs.x) * 0.5f, bp.y + (bs - qs.y) * 0.5f), U(hovered ? kAccent : kMuted), "?");
            if (hovered) Tip("打开 LS 缩放方案速查：每种缩放方案是干什么的、适合什么。");
            if (clicked) ImGui::OpenPopup(kScalingHelpPopup);
        }
    }

    // the line under the band
    dl->AddLine(ImVec2(origin.x, origin.y + height - 1), ImVec2(origin.x + width, origin.y + height - 1), U(kBorder), 1.0f);
    ImGui::Dummy(ImVec2(width, height));

    // the details, when the pointer rests on the readout
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (!readings.empty() && ImGui::IsWindowHovered() && mouse.x >= readoutLeft && mouse.x <= origin.x + width && mouse.y >= origin.y && mouse.y <= origin.y + height) {
        std::string tip;
        char line[160];
        if (g.ok) {
            snprintf(line, sizeof line, "%s\n负载 %u%%，温度 %u °C，功耗 %.0f / %.0f W\n核心 %u MHz，显存 %u MHz\n已用显存 %s\n\n", g.name.c_str(), g.utilGpu, g.tempC,
                     g.powerW, g.powerLimitW, g.clockGraphics, g.clockMem, Gigabytes(g.vramUsedMB, g.vramTotalMB).c_str());
            tip += line;
        } else if (!g.why.empty()) {
            tip += "显卡：" + g.why + "\n\n";
        }
        if (s.ok) {
            snprintf(line, sizeof line, "系统内存已用 %s\n处理器占用 %.0f%%（所有核心）", Gigabytes(s.ramUsedMB, s.ramTotalMB).c_str(), s.cpuPercent);
            tip += line;
        }
        tip += "\n\n历史记录及其含义见“性能”页。";
        Tip(tip.c_str());
    }

    // the LS scaling cheat-sheet, opened by the ? in the header bar: centred, fixed size, stretch
    // columns (fixed-width cells refused to wrap on real installs); the window itself scrolls if needed
    const float fs = ImGui::GetFontSize();
    const float maxW = ImGui::GetMainViewport()->WorkSize.x * 0.92f;
    const float w = fs * 36.0f < maxW ? fs * 36.0f : maxW;
    const float maxH = ImGui::GetMainViewport()->WorkSize.y * 0.85f;
    const float h = fs * 24.0f < maxH ? fs * 24.0f : maxH;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(kScalingHelpPopup, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(0.0f);
        WarnLine("插件联动：DLSS 5 / 上采样插件只接管 NIS 的输出——插件开着时选了其他缩放类型，插件不会生效！这是“插件怎么没反应”的头号原因。");
        ImGui::Dummy(ImVec2(0, S(4)));
        WarnLine("像素画 / 2D 游戏别配神经渲染（NR）：平涂画面没有细节可恢复，只会换来闪烁和额外功耗。");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(6)));

        if (ImGui::BeginTable("##scaling_schemes", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("方案", ImGuiTableColumnFlags_WidthStretch, 9.2f);
            ImGui::TableSetupColumn("是干什么的", ImGuiTableColumnFlags_WidthStretch, 11.6f);
            ImGui::TableSetupColumn("适合什么", ImGuiTableColumnFlags_WidthStretch, 12.8f);
            ImGui::TableHeadersRow();
            auto cell = [](const char* text) {   // wrap at whatever width the stretch column gave the cell
                const float avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().CellPadding.x;
                if (avail > 0.0f) ImGui::PushTextWrapPos(ImGui::GetCursorScreenPos().x + avail);
                ImGui::TextUnformatted(text);
                if (avail > 0.0f) ImGui::PopTextWrapPos();
            };
            for (const ScalingRow& r : kScalingRows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                cell(r.scheme);
                ImGui::TableNextColumn();
                cell(r.what);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, (&r == &kScalingRows[0] ? eam::ui::theme::V(eam::ui::theme::kAccent) : ImGui::GetStyleColorVec4(ImGuiCol_Text)));
                cell(r.suits);
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }

        ImGui::Dummy(ImVec2(0, S(2)));
        if (ui::Button("知道了", ui::icons::kCheck, ui::ButtonKind::Primary)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace widgets
} // namespace eam
