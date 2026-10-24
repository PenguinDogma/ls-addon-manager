#include "runtime_list.h"
#include "file_dialog.h"
#include "toast.h"
#include "imgui.h"
#include <eam/icons.h>
#include <eam/widgets.h>
#include <string>
#include <windows.h>

namespace eam {
namespace widgets {

namespace {
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
} // namespace

RuntimeAction RuntimeListAtBottom(const std::vector<RuntimeFile>& rows, int openMenu) {
    RuntimeAction action;
    if (rows.empty()) return action;
    namespace th = ui::theme;
    const float a = ImGui::GetStyle().Alpha;
    // its height as drawn the frame before (an estimate the first time): the room above it is filled so it ends at the window's bottom
    static float measured = 0.0f;
    const float need = measured > 0.0f ? measured : ImGui::GetFontSize() * 2.0f + rows.size() * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y);
    const float gap = ImGui::GetContentRegionAvail().y - need - ImGui::GetStyle().ItemSpacing.y;
    if (gap > 0.0f) ImGui::Dummy(ImVec2(0, gap));
    const float top = ImGui::GetCursorPosY();
    ImGui::PushStyleColor(ImGuiCol_Separator, th::V(th::kBorder)); ImGui::Separator(); ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.2f));
    ui::SectionLabel("运行时");
    ImGui::PushID("##runtimes");   // its lines are numbered from 0 like the addon cards above them in the same window: their own ID scope
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.15f));
    for (int i = 0; i < (int)rows.size(); ++i) {
        const RuntimeFile& r = rows[i];
        ImGui::PushID(i);
        const float h = ImGui::GetFrameHeight(), w = ImGui::GetContentRegionAvail().x;
        const float buttons = h;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // the line itself: hovering it shows the details
        ImGui::InvisibleButton("##row", ImVec2(w - buttons, h));
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort);
        if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + w - buttons, p.y + h), th::U(th::kRowHover, a), 3.0f);
        const float ico = ImGui::GetFontSize() * 0.9f;
        // a tick: loaded now; a circle: its addon is on and loads it when Lossless Scaling scales a game; a cross: its addon is off (or no file)
        const bool waiting = !r.loaded && r.addonOn && r.exists;
        const char* glyph = r.loaded ? ui::icons::kCheck : waiting ? ui::icons::kCircle : ui::icons::kClose;
        const ImU32 tint = r.loaded ? th::U(th::kAccent, a) : th::U(th::kMuted, a);
        std::string note;
        if (!r.exists) note = "未找到";
        else if (!r.read) note = "...";
        else if (r.signature == RuntimeFile::Signature::Broken) note = "已修改";
        else if (r.signature != RuntimeFile::Signature::Signed) note = "未签名";
        ui::svg::Draw(dl, glyph, ImVec2(p.x + 2.0f, p.y + (h - ico) * 0.5f), ico, tint, 2.0f);
        float x = p.x + ico + ImGui::GetFontSize() * 0.55f;
        const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f, right = p.x + w - buttons - 4.0f;
        const ImVec4 clip(p.x, p.y, right, p.y + h);
        dl->AddText(nullptr, 0.0f, ImVec2(x, ty), th::U(th::kText, a), r.label.c_str(), nullptr, 0.0f, &clip);
        x += ImGui::CalcTextSize(r.label.c_str()).x + ImGui::GetFontSize() * 0.4f;
        const std::string version = r.exists && r.read ? r.ShownVersion() : std::string();
        if (!version.empty()) {
            dl->AddText(nullptr, 0.0f, ImVec2(x, ty), th::U(th::kMuted, a), version.c_str(), nullptr, 0.0f, &clip);
            x += ImGui::CalcTextSize(version.c_str()).x + ImGui::GetFontSize() * 0.4f;
        }
        if (!note.empty()) dl->AddText(nullptr, 0.0f, ImVec2(x, ty), th::U(th::kText, a * 0.75f), note.c_str(), nullptr, 0.0f, &clip);

        if (hovered) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::Text("%s 运行时，由 %s 使用（%s）", r.label.c_str(), r.addonName.c_str(), r.addonOn ? "开" : "关");
            if (r.loaded) ImGui::TextColored(th::V(th::kAccent), "当前已加载");
            else if (waiting) ImGui::TextDisabled("就绪：开启 %s 后，Lossless Scaling 缩放游戏时会自动加载", r.addonName.c_str());
            else if (!r.exists) ImGui::TextDisabled("未加载：文件不存在");
            else ImGui::TextDisabled("未加载：%s 未开启", r.addonName.c_str());
            if (!r.exists) ImGui::Text("未找到：%s", Utf8(r.path).c_str());
            else if (!r.read) ImGui::TextDisabled("正在读取文件…");
            else {
                if (!r.description.empty() || !r.version.empty()) ImGui::TextUnformatted((r.description + (r.description.empty() ? "" : "  ") + r.version).c_str());
                if (r.signature == RuntimeFile::Signature::Signed) ImGui::Text("签名者：%s", r.signer.empty() ? "其开发商" : r.signer.c_str());
                else if (r.signature == RuntimeFile::Signature::Broken)
                    ImGui::Text("已修改：由 %s 签名后文件被改动，签名与文件不再相符", r.signer.empty() ? "其开发商" : r.signer.c_str());
                else ImGui::Text("未签名%s", r.company.empty() ? "" : ("（自称来自 " + r.company + "）").c_str());
                if (r.shippedKnown) ImGui::TextDisabled(r.shipped ? "插件自带的文件。" : "并非插件自带的文件。");
                ImGui::TextDisabled("%s", Utf8(r.path).c_str());
                if (!r.sha256.empty()) ImGui::TextDisabled("SHA-256 %s", r.sha256.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        // + : the files there are for it, one click to switch, and adding one
        ImGui::SameLine(0, 0);
        if (ui::IconButton("##choose", ui::icons::kPlus, h)) { action = { i, RuntimeAction::Choose }; ImGui::OpenPopup("##files"); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("切换 %s 文件", r.label.c_str());
        if (i == openMenu && !ImGui::IsPopupOpen("##files")) ImGui::OpenPopup("##files");
        // the menu opens beside the list, its bottom at this line (not wherever the pointer is)
        ImGui::SetNextWindowPos(ImVec2(p.x + w + ImGui::GetStyle().ItemSpacing.x, p.y + h), ImGuiCond_Appearing, ImVec2(0.0f, 1.0f));
        if (ImGui::BeginPopup("##files")) {
            ui::SectionLabel((r.label + " 文件").c_str());
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.2f));
            auto line = [&](const RuntimeFile& f, const std::string& name) {
                std::string text = name;
                const std::string v = f.exists && f.read ? f.ShownVersion() : std::string();
                if (!v.empty()) text += "  " + v;
                if (!f.exists) text += "  （缺失）";
                else if (f.read && f.signature == RuntimeFile::Signature::Broken) text += "  已修改";
                else if (f.read && f.signature != RuntimeFile::Signature::Signed) text += "  未签名";
                return text;
            };
            const bool usingDefault = r.usingDefault;
            // the default first: always there, so there is always a way back
            {
                const RuntimeFile def = DescribeRuntimeFile(r.defaultPath, r);
                if (ImGui::Selectable(line(def, r.shippedKnown ? "自带" : "默认").c_str(), usingDefault)
                    && !usingDefault) {
                    UseRuntimeFile(r, L"");
                    ToastShow(r.label + "：已恢复" + (r.shippedKnown ? "自带" : "默认") + "文件", ToastType::Success, 4.0f);
                }
            }
            const std::vector<std::wstring> library = RuntimeLibrary(r);
            for (size_t k = 0; k < library.size(); ++k) {
                ImGui::PushID((int)k);
                const RuntimeFile f = DescribeRuntimeFile(library[k], r);
                const bool current = !usingDefault && _wcsicmp(library[k].c_str(), r.path.c_str()) == 0;
                const float trash = ImGui::GetFrameHeight();
                const std::string title = RuntimeFileTitle(library[k]);
                if (ImGui::Selectable((title.empty() ? line(f, r.label) : title + (f.read && f.signature == RuntimeFile::Signature::Signed ? "" : "  未签名")).c_str(), current,
                                      ImGuiSelectableFlags_DontClosePopups, ImVec2(ImGui::GetFontSize() * 16.0f, 0)) && !current) {
                    UseRuntimeFile(r, library[k]);
                    ToastShow(r.label + "：已切换到 " + (f.ShownVersion().empty() ? std::string("新文件") : f.ShownVersion()), ToastType::Success, 4.0f);
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    if (!f.description.empty()) ImGui::TextUnformatted((f.description + "  " + f.version).c_str());
                    if (f.signature == RuntimeFile::Signature::Signed) ImGui::Text("签名者：%s", f.signer.c_str()); else ImGui::TextUnformatted(f.signature == RuntimeFile::Signature::Broken ? "签名后已被修改" : "未签名");
                    if (!f.sha256.empty()) ImGui::TextDisabled("SHA-256 %s", f.sha256.c_str());
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();
                if (ui::IconButton("##remove", ui::icons::kTrash, trash)) {
                    std::string error;
                    if (RemoveRuntimeFile(r, library[k], error)) ToastShow(r.label + "：文件已移除（移到了 runtimes\\.removed）", ToastType::Info, 4.0f);
                    else ToastShow(error, ToastType::Warning, 5.0f);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("从此列表中移除");
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Selectable("+  添加文件…")) {
                std::wstring picked;
                const std::wstring title = L"选择 " + std::wstring(r.label.begin(), r.label.end()) + L" 文件";
                if (PickOpenFile(title.c_str(), L"DLL 文件", L"*.dll", picked)) {
                    std::wstring added; std::string error;
                    if (AddRuntimeFile(r, picked, added, error)) {
                        UseRuntimeFile(r, added);
                        const RuntimeFile f = DescribeRuntimeFile(added, r);
                        ToastShow(r.label + "：已添加并切换" + (f.ShownVersion().empty() ? std::string() : "到 " + f.ShownVersion()), ToastType::Success, 4.0f);
                    } else ToastShow(error, ToastType::Warning, 6.0f);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    measured = ImGui::GetCursorPosY() - top;
    return action;
}

} // namespace widgets
} // namespace eam
