#include "ls_config.h"
#include "../log/logger.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace eam {
namespace features {
namespace lsconfig {

namespace {

std::string Trim(std::string s) {
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out; std::string line; std::istringstream in(text);
    while (std::getline(in, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); out.push_back(line); }
    return out;
}

bool IsSection(const std::string& line, std::string* name = nullptr) {
    const std::string t = Trim(line);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return false;
    if (name) *name = Trim(t.substr(1, t.size() - 2));
    return true;
}

// The key of a "key = value" line, or empty.
std::string KeyOf(const std::string& line) {
    const std::string t = Trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return {};
    const size_t eq = t.find('=');
    return eq == std::string::npos ? std::string() : Trim(t.substr(0, eq));
}

fs::path ConfigPath() {   // beside Lossless.dll (Lossless Scaling's own folder)
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ConfigPath), &self);
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH * 2);
    return n ? fs::path(std::wstring(path, n)).parent_path() / L"config.ini" : fs::path(L"config.ini");
}

std::string ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

bool WriteSetting(const char* section, const char* key, const std::string& value) {
    const fs::path path = ConfigPath();
    const std::string before = ReadFile(path);
    std::error_code ec;
    const fs::path backup = path.parent_path() / L"config.ini.bak-manager";
    if (!fs::exists(backup, ec) && fs::exists(path, ec)) fs::copy_file(path, backup, ec);   // (once: the file as it was before the manager touched it)
    const std::string after = IniSet(before, section, key, value);
    if (after == before) return true;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << after;
    if (!out) { LOG_WARN("LsConfig", "config.ini could not be written (%ls)", path.c_str()); return false; }
    LOG_INFO("LsConfig", "config.ini: [%s] %s = %s", section, key, value.c_str());
    return true;
}

std::string Get(const char* section, const char* key, const char* fallback) {
    static std::string text; static ULONGLONG at = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - at > 1000 || text.empty()) { text = ReadFile(ConfigPath()); at = now; }   // (the window asks every frame)
    return IniGet(text, section, key, fallback);
}
void Dirty() { /* the next Get re-reads after a second */ }

bool EditFloat(const char* label, const char* section, const char* key, const char* fallback, float lo, float hi, const char* tip) {
    float v = static_cast<float>(atof(Get(section, key, fallback).c_str()));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    bool changed = false;
    if (ImGui::SliderFloat(label, &v, lo, hi, "%.2f")) { char t[32]; snprintf(t, sizeof t, "%.2f", v); WriteSetting(section, key, t); Dirty(); changed = true; }
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return changed;
}
bool EditInt(const char* label, const char* section, const char* key, const char* fallback, int lo, int hi, const char* tip) {
    int v = atoi(Get(section, key, fallback).c_str());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    bool changed = false;
    if (ImGui::SliderInt(label, &v, lo, hi)) { WriteSetting(section, key, std::to_string(v)); Dirty(); changed = true; }
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return changed;
}
bool EditBool(const char* label, const char* section, const char* key, const char* fallback, const char* tip) {
    bool v = atoi(Get(section, key, fallback).c_str()) != 0;
    bool changed = false;
    if (ImGui::Checkbox(label, &v)) { WriteSetting(section, key, v ? "1" : "0"); Dirty(); changed = true; }
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return changed;
}

} // namespace

std::string IniGet(const std::string& text, const std::string& section, const std::string& key, const std::string& fallback) {
    bool in = false;
    for (const std::string& line : Lines(text)) {
        std::string name;
        if (IsSection(line, &name)) { in = name == section; continue; }
        if (in && KeyOf(line) == key) return Trim(line.substr(line.find('=') + 1));
    }
    return fallback;
}

std::string IniSet(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
    std::vector<std::string> lines = Lines(text);
    const bool crlf = text.find("\r\n") != std::string::npos || text.empty();
    bool in = false, sectionSeen = false, done = false;
    size_t lastInSection = 0;
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        std::string name;
        if (IsSection(lines[i], &name)) { if (in) break; in = name == section; if (in) { sectionSeen = true; lastInSection = i; } continue; }
        if (!in) continue;
        if (!Trim(lines[i]).empty()) lastInSection = i;
        if (KeyOf(lines[i]) == key) { lines[i] = key + " = " + value; done = true; }
    }
    if (!done) {
        if (sectionSeen) lines.insert(lines.begin() + lastInSection + 1, key + " = " + value);
        else { if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back(""); lines.push_back("[" + section + "]"); lines.push_back(key + " = " + value); }
    }
    const char* eol = crlf ? "\r\n" : "\n";
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) { out += lines[i]; if (i + 1 < lines.size()) out += eol; }
    if (text.empty() || text.back() == '\n') out += eol;   // (a file that ended with a line break still does)
    return out;
}

void RenderOptions() {
    ImGui::TextDisabled("Lossless Scaling 自己的 config.ini 中、其窗口没有显示的设置。改动从下一次缩放开始时生效。");
    ImGui::Spacing();
    ImGui::TextUnformatted("Lossless Scaling 的帧率计数器");
    EditFloat("大小", "fps_counter", "scale", "1.0", 0.5f, 4.0f, "计数器文字的大小（1.00 为 Lossless Scaling 的默认大小）。");
    {   // colour: 0xRRGGBB in the file
        const std::string c = Get("fps_counter", "color", "0xD3D3D3");
        const unsigned rgb = static_cast<unsigned>(strtoul(c.c_str(), nullptr, 0));
        float col[3] = { ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::ColorEdit3("颜色", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            char t[16]; snprintf(t, sizeof t, "0x%02X%02X%02X", int(col[0] * 255 + 0.5f), int(col[1] * 255 + 0.5f), int(col[2] * 255 + 0.5f));
            WriteSetting("fps_counter", "color", t); Dirty();
        }
        ImGui::SameLine(); ImGui::TextUnformatted("颜色");
    }
    {   // font name
        static char font[64]; static bool loaded = false;
        if (!loaded) { snprintf(font, sizeof font, "%s", Get("fps_counter", "font", "Consolas").c_str()); loaded = true; }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::InputText("字体", font, sizeof font, ImGuiInputTextFlags_EnterReturnsTrue)) { WriteSetting("fps_counter", "font", font); Dirty(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("已安装字体的名称（按回车保存）。Lossless Scaling 默认为 Consolas。");
    }
    EditInt("向右移动", "fps_counter", "offset_x", "0", -400, 400, "相对计数器默认位置（左上角）的像素偏移（负值：向左）。");
    EditInt("向下移动", "fps_counter", "offset_y", "0", -400, 400, "相对计数器默认位置（左上角）的像素偏移（负值：向上）。");
    EditBool("显示两个数字", "fps_counter", "show_captured", "1", "开启：显示\"捕获 / 显示\"（60 / 120）。关闭：仅显示输出的帧率。");
    ImGui::Spacing();
    if (ImGui::TreeNode("专家数值（Lossless Scaling 原名；一次只改一个）")) {
        ImGui::TextDisabled("捕获");
        EditInt("frametime_buffer_size", "capture", "frametime_buffer_size", "15", 1, 120, nullptr);
        EditInt("frametime_buffer_reset_multiplier", "capture", "frametime_buffer_reset_multiplier", "6", 1, 60, nullptr);
        EditFloat("queue_draining_momentum", "capture", "queue_draining_momentum", "0.01", 0.0f, 1.0f, nullptr);
        ImGui::TextDisabled("帧生成");
        EditInt("ui_detection_rate", "lsfg", "ui_detection_rate", "1", 0, 8, nullptr);
        EditFloat("ui_detection_threshold", "lsfg", "ui_detection_threshold", "0.5", 0.0f, 1.0f, nullptr);
        EditFloat("real_timestamp_tolerance", "lsfg", "real_timestamp_tolerance", "0.05", 0.0f, 1.0f, nullptr);
        EditInt("base_framerate_threshold", "lsfg", "base_framerate_threshold", "10", 0, 120, nullptr);
        ImGui::TextDisabled("渲染");
        EditBool("flush", "rendering", "flush", "1", nullptr);
        ImGui::TextDisabled("文件中缺少某个键时显示的数值是 Lossless Scaling 的默认值。config.ini.bak-manager 是管理器首次修改之前的原文件。");
        ImGui::TreePop();
    }
}

} // namespace lsconfig
} // namespace features
} // namespace eam
