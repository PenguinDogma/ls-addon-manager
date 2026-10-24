#include "tab_performance.h"
#include "../gui_scale.h"
#include "../widgets/empty_state.h"
#include "../widgets/tooltip.h"
#include "../../host/metrics.h"
#include "../../host/gpu_stats.h"
#include "imgui.h"
#include "eam/widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace eam {

namespace {

// The addons that report the game's frame times, and what each calls its cost a frame (Neural Rendering: the model; the upscalers: themselves).
struct Reporter { const char* id; const char* costKey; const char* costName; };
constexpr Reporter kReporters[] = {
    { "DLSS5NR01", "model_ms", "模型开销" }, { "DLSS4DLAA", "dlss_ms", "上采样器开销" },
    { "FSR3UPSC", "fsr_ms", "上采样器开销" }, { "XESSUPSC", "xess_ms", "上采样器开销" },
};
constexpr const char* kNr = "DLSS5NR01";
constexpr double kGraphSeconds = 20.0;   // the length of the graphs
constexpr double kStatSeconds = 10.0;    // what the tiles average over

using eam::ui::theme::U;
using eam::ui::theme::V;

struct Stats { float avg = 0, mn = 0, mx = 0, p50 = 0, p95 = 0, p99 = 0; int n = 0; };

Stats Summarise(const Metrics::Series& s, double now, double seconds) {
    Stats st;
    std::vector<float> v;
    for (const auto& x : s.samples) if (x.t >= now - seconds) v.push_back(x.v);
    st.n = (int)v.size();
    if (v.empty()) return st;
    double sum = 0; st.mn = st.mx = v[0];
    for (float x : v) { sum += x; st.mn = (std::min)(st.mn, x); st.mx = (std::max)(st.mx, x); }
    st.avg = (float)(sum / v.size());
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[(size_t)((std::min)((double)v.size() - 1, std::floor(p * (v.size() - 1) + 0.5)))]; };
    st.p50 = pct(0.50); st.p95 = pct(0.95); st.p99 = pct(0.99);
    return st;
}

std::vector<ImVec2> ToPoints(const Metrics::Series& s, double now, double seconds) {
    std::vector<ImVec2> pts;
    pts.reserve(s.samples.size());
    for (const auto& x : s.samples) {
        const double t = 1.0 - (now - x.t) / seconds;
        if (t >= 0.0 && t <= 1.0) pts.push_back(ImVec2((float)t, x.v));
    }
    return pts;
}

// A rounded tile with a caption, a big number and a small line under it.
void Tile(const char* id, float width, const char* caption, const char* big, const char* sub, ImU32 bigColour) {
    const float h = ImGui::GetFontSize() * 5.4f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("##tile", ImVec2(width, h));
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), U(eam::ui::theme::kPanel), 4.0f);
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), U(eam::ui::theme::kBorder), 4.0f, 0, 1.0f);
    const float pad = ImGui::GetFontSize() * 0.7f;
    dl->AddText(ImVec2(p.x + pad, p.y + pad * 0.8f), U(eam::ui::theme::kAccentDim), caption);
    ImFont* font = ImGui::GetFont();
    dl->AddText(font, ImGui::GetFontSize() * 1.9f, ImVec2(p.x + pad, p.y + pad * 0.8f + ImGui::GetFontSize() * 1.15f), bigColour, big);
    dl->AddText(ImVec2(p.x + pad, p.y + h - pad - ImGui::GetTextLineHeight() * 0.9f), U(eam::ui::theme::kMuted), sub);
}

// A horizontal bar with a caption on the left and the value on the right.
void Bar(const char* caption, float fraction, const char* value, ImU32 colour) {
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 0.55f;
    const float capW = ImGui::GetFontSize() * 6.5f, valW = ImGui::GetFontSize() * 9.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    dl->AddText(ImVec2(p.x, p.y), U(eam::ui::theme::kMuted), caption);
    const float x0 = p.x + capW, x1 = p.x + w - valW, cy = p.y + ImGui::GetTextLineHeight() * 0.5f;
    dl->AddRectFilled(ImVec2(x0, cy - h * 0.5f), ImVec2(x1, cy + h * 0.5f), U(eam::ui::theme::kBorderBright), h * 0.5f);
    const float f = fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);
    if (f > 0.001f) dl->AddRectFilled(ImVec2(x0, cy - h * 0.5f), ImVec2(x0 + (x1 - x0) * f, cy + h * 0.5f), colour, h * 0.5f);
    dl->AddText(ImVec2(x1 + ImGui::GetFontSize() * 0.6f, p.y), U(eam::ui::theme::kText), value);
    ImGui::Dummy(ImVec2(w, rowH));
}

void Reading(ImU32 dot, const std::string& text) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = ImGui::GetFontSize() * 0.22f;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r + 2, p.y + ImGui::GetTextLineHeight() * 0.5f), r, dot);
    ImGui::SetCursorScreenPos(ImVec2(p.x + ImGui::GetFontSize() * 1.1f, p.y));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
}

} // namespace

void RenderTabPerformance() {
    GpuStats::Instance().Wanted();
    Metrics& M = Metrics::Instance();
    const double now = M.Now();
    // the addon whose frame times are the newest (with several on, the one working on the frames now)
    const Reporter* rep = &kReporters[0];
    Metrics::Series frame;
    for (const Reporter& r : kReporters) {
        Metrics::Series s = M.Get(r.id, "frame_ms", kGraphSeconds + 1);
        if (s.samples.empty()) continue;
        if (frame.samples.empty() || s.samples.back().t > frame.samples.back().t) { frame = std::move(s); rep = &r; }
    }
    const Metrics::Series model = M.Get(rep->id, rep->costKey, kGraphSeconds + 1);
    const Metrics::Series keep = M.Get(kNr, "keepup_pct", kGraphSeconds + 1);
    const bool ownModel = rep == &kReporters[0];   // "keeps up" is Neural Rendering's; an upscaler runs on every frame it is given
    const Metrics::Series util = M.Get("system", "gpu_util", kGraphSeconds + 1);
    const Metrics::Series power = M.Get("system", "gpu_power_w", kGraphSeconds + 1);
    const GpuStats::Snapshot gpu = GpuStats::Instance().Get();
    const bool haveFrames = !frame.samples.empty() && now - frame.samples.back().t < 5.0;
    const bool haveGpu = gpu.ok && gpu.ageSeconds < 5.0;

    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::BeginChild("##perf", ImVec2(0, 0), false);

    if (!haveFrames && !haveGpu) {
        widgets::EmptyState(ImGui::GetContentRegionAvail().x, "暂无实时数据",
                            gpu.ok ? "正在等待 GPU…" : "请开启某个插件（神经渲染或上采样器）并对游戏进行缩放。此处会显示游戏的帧时间以及插件的开销。");
        if (!gpu.ok && !gpu.why.empty()) { ImGui::Dummy(ImVec2(0, S(6))); ImGui::TextDisabled("GPU：%s", gpu.why.c_str()); }
        ImGui::EndChild();
        return;
    }

    const Stats fs = Summarise(frame, now, kStatSeconds), ms = Summarise(model, now, kStatSeconds), ks = Summarise(keep, now, kStatSeconds);
    const Stats us = Summarise(util, now, kStatSeconds);

    // ---- tiles
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float tileW = (ImGui::GetContentRegionAvail().x - gap * 3.0f) / 4.0f;
    char big[64], sub[96];
    if (haveFrames && fs.avg > 0.1f) snprintf(big, sizeof big, "%.0f fps", 1000.0f / fs.avg); else snprintf(big, sizeof big, "-");
    snprintf(sub, sizeof sub, haveFrames ? "平均 %.1f ms" : "暂无帧数据", fs.avg);
    Tile("t1", tileW, "游戏帧率", big, sub, U(eam::ui::theme::kAccent));
    ImGui::SameLine();
    if (haveFrames) snprintf(big, sizeof big, "%.1f ms", fs.p95); else snprintf(big, sizeof big, "-");
    snprintf(sub, sizeof sub, haveFrames ? "最差帧 %.0f ms" : "", fs.mx);
    Tile("t2", tileW, "最慢 5% 的帧", big, sub, U(fs.p95 > 25.0f ? eam::ui::theme::kWarn : eam::ui::theme::kAccent));
    ImGui::SameLine();
    if (!model.samples.empty()) snprintf(big, sizeof big, "%.1f ms", ms.avg); else snprintf(big, sizeof big, "-");
    snprintf(sub, sizeof sub, ownModel && !keep.samples.empty() ? "跟得上 %.0f%%" : "", ks.avg);
    Tile("t3", tileW, rep->costName, big, sub, U(ownModel && ks.n && ks.avg < 90.0f ? eam::ui::theme::kWarn : eam::ui::theme::kAccent));
    ImGui::SameLine();
    if (haveGpu) snprintf(big, sizeof big, "%u%%", gpu.utilGpu); else snprintf(big, sizeof big, "-");
    if (haveGpu && gpu.hasPower) snprintf(sub, sizeof sub, "%.0f / %.0f W", gpu.powerW, gpu.powerLimitW);
    else if (haveGpu && gpu.vramTotalMB) snprintf(sub, sizeof sub, "显存 %llu / %llu MB", (unsigned long long)gpu.vramUsedMB, (unsigned long long)gpu.vramTotalMB);
    else sub[0] = 0;
    Tile("t4", tileW, "GPU 负载", big, sub, U(haveGpu && gpu.utilGpu >= 95 ? eam::ui::theme::kWarn : eam::ui::theme::kAccent));

    // ---- frame time graph
    ImGui::Dummy(ImVec2(0, S(6)));
    eam::ui::SectionLabel("帧时间（最近 20 秒）");
    {
        const std::vector<ImVec2> pts = ToPoints(frame, now, kGraphSeconds);
        const float top = (std::max)(40.0f, fs.mx * 1.1f);
        const float guides[2] = { 16.7f, 33.3f };
        const char* labels[2] = { "60 fps", "30 fps" };
        if (pts.size() > 1)
            eam::ui::LineGraph("frames", pts.data(), (int)pts.size(), ImVec2(0, ImGui::GetFontSize() * 7.0f), 0.0f, top, U(eam::ui::theme::kAccent), guides, labels, 2, 33.3f, U(eam::ui::theme::kWarn), "ms");
        else { ImGui::TextDisabled("最近几秒没有帧数据。"); }
    }
    widgets::Tip("Lossless Scaling 所见的游戏真实帧间隔时间。平坦的 16.7 ms 直线即稳定 60 fps。高于上方虚线（33 ms）的尖峰会变为琥珀色。");

    // ---- model and GPU graphs, side by side
    ImGui::Dummy(ImVec2(0, S(6)));
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    ImGui::BeginGroup();
    eam::ui::SectionLabel(ownModel ? "模型开销（ms）" : "上采样器开销（ms）");
    {
        const std::vector<ImVec2> pts = ToPoints(model, now, kGraphSeconds);
        if (pts.size() > 1) eam::ui::LineGraph("model", pts.data(), (int)pts.size(), ImVec2(half, ImGui::GetFontSize() * 5.0f), 0.0f, (std::max)(10.0f, ms.mx * 1.2f), U(eam::ui::theme::kAccent), nullptr, nullptr, 0, 0, 0, "ms");
        else ImGui::TextDisabled("尚未上报。");
    }
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    eam::ui::SectionLabel("GPU 负载（%）");
    {
        const std::vector<ImVec2> pts = ToPoints(util, now, kGraphSeconds);
        const float guides[1] = { 95.0f };
        if (pts.size() > 1) eam::ui::LineGraph("gpu", pts.data(), (int)pts.size(), ImVec2(half, ImGui::GetFontSize() * 5.0f), 0.0f, 100.0f, U(eam::ui::theme::kAccent), guides, nullptr, 1, 95.0f, U(eam::ui::theme::kWarn), "%");
        else ImGui::TextDisabled("正在等待 GPU…");
    }
    ImGui::EndGroup();

    // ---- GPU details
    if (haveGpu) {
        ImGui::Dummy(ImVec2(0, S(8)));
        char t[8][96];
        snprintf(t[7], sizeof t[7], "GPU  %s%s", gpu.name.c_str(), gpu.deviceCount > 1 ? "  （多块中的第一块）" : "");
        eam::ui::SectionLabel(t[7]);
        const bool atCap = gpu.hasPower && gpu.powerLimitW > 0 && gpu.powerW >= gpu.powerLimitW * 0.96;
        snprintf(t[0], sizeof t[0], "%u%%", gpu.utilGpu);
        Bar("负载", gpu.utilGpu / 100.0f, t[0], U(gpu.utilGpu >= 95 ? eam::ui::theme::kWarn : eam::ui::theme::kAccent));
        if (gpu.hasPower) {
            snprintf(t[1], sizeof t[1], "%.0f / %.0f W", gpu.powerW, gpu.powerLimitW);
            Bar("功耗", gpu.powerLimitW > 0 ? (float)(gpu.powerW / gpu.powerLimitW) : 0.0f, t[1], U(atCap ? eam::ui::theme::kWarn : eam::ui::theme::kAccent));
        }
        if (gpu.vramTotalMB) {
            snprintf(t[2], sizeof t[2], "%llu / %llu MB", (unsigned long long)gpu.vramUsedMB, (unsigned long long)gpu.vramTotalMB);
            Bar("显存", (float)gpu.vramUsedMB / (float)gpu.vramTotalMB, t[2], U(eam::ui::theme::kAccent));
        }
        if (gpu.hasTemp) {
            snprintf(t[3], sizeof t[3], "%u C", gpu.tempC);
            Bar("温度", gpu.tempC / 100.0f, t[3], U(gpu.tempC >= 85 ? eam::ui::theme::kWarn : eam::ui::theme::kAccent));
        }
        if (gpu.hasClocks) {
            snprintf(t[4], sizeof t[4], "核心 %u MHz，显存 %u MHz", gpu.clockGraphics, gpu.clockMem);
            ImGui::TextDisabled("频率"); ImGui::SameLine(ImGui::GetFontSize() * 6.5f); ImGui::TextUnformatted(t[4]);
        }
        if (gpu.hasThrottle) {
            const std::string th = GpuStats::ThrottleText(gpu.throttle);
            ImGui::TextDisabled("受限于"); ImGui::SameLine(ImGui::GetFontSize() * 6.5f);
            if (th.empty()) ImGui::TextUnformatted("无"); else { ImGui::PushStyleColor(ImGuiCol_Text, V(eam::ui::theme::kWarn)); ImGui::TextUnformatted(th.c_str()); ImGui::PopStyleColor(); }
        }
        if (gpu.viaCounters) ImGui::TextDisabled("负载与显存来自 Windows 自带计数器（与任务管理器相同）；此显卡的功耗、频率与温度未上报。");
    }

    // ---- what it says
    ImGui::Dummy(ImVec2(0, S(8)));
    eam::ui::SectionLabel("数据解读");
    bool said = false;
    if (haveGpu && gpu.hasPower && gpu.powerLimitW > 0 && gpu.powerW >= gpu.powerLimitW * 0.96 && gpu.utilGpu >= 90) {
        char b[220]; snprintf(b, sizeof b, "GPU 已达功耗上限（%.0f W）且满载，无法再快。调低游戏渲染缩放或模型分辨率可释放余量。", gpu.powerLimitW);
        Reading(U(eam::ui::theme::kWarn), b); said = true;
    } else if (haveGpu && gpu.hasThrottle && (gpu.throttle & 0x60) && gpu.utilGpu >= 80) {
        Reading(U(eam::ui::theme::kWarn), "GPU 正因温度而降速。请检查机箱风道与风扇曲线。"); said = true;
    }
    if (ownModel && ks.n && ks.avg < 90.0f) {
        char b[200]; snprintf(b, sizeof b, "模型仅在 %.0f%% 的帧上运行：增强处理跟不上画面。请在插件设置中调低模型分辨率。", ks.avg);
        Reading(U(eam::ui::theme::kWarn), b); said = true;
    }
    if (haveFrames && fs.n > 30) {
        if (fs.p95 > 1.5f * fs.p50 && fs.p95 > 22.0f) {
            char b[200]; snprintf(b, sizeof b, "帧节奏不均：一半帧耗时 %.1f ms，但最慢的 5%% 达 %.1f ms 以上，表现为卡顿。", fs.p50, fs.p95);
            Reading(U(eam::ui::theme::kWarn), b);
        } else {
            char b[200]; snprintf(b, sizeof b, "帧时间平稳：一半帧在 %.1f ms 内，95%% 在 %.1f ms 以内。", fs.p50, fs.p95);
            Reading(U(eam::ui::theme::kAccent), b);
        }
        said = true;
    }
    if (!said) ImGui::TextDisabled("暂无提示。");

    // ---- every live value
    ImGui::Dummy(ImVec2(0, S(8)));
    if (eam::ui::SectionHeader("全部实时数值")) {
        if (ImGui::BeginTable("##allmetrics", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("来源"); ImGui::TableSetupColumn("数值"); ImGui::TableSetupColumn("当前"); ImGui::TableSetupColumn("平均"); ImGui::TableSetupColumn("最小"); ImGui::TableSetupColumn("最大");
            ImGui::TableHeadersRow();
            for (const auto& s : M.Snapshot(kStatSeconds)) {
                if (s.samples.empty()) continue;
                const Stats st = Summarise(s, now, kStatSeconds);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.addon.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.key.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.2f %s", s.last(), s.unit.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.2f", st.avg);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", st.mn);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", st.mx);
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

} // namespace eam
