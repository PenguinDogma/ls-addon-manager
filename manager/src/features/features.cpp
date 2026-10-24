#include "features.h"
#include "gpu_limit.h"
#include "ls_config.h"
#include "superlog.h"
#include "recorder_feature.h"
#include "reshade_passthrough.h"
#include "windowed_mode.h"
#include "../config/config_manager.h"
#include "../log/logger.h"
#include <cstring>

namespace eam {
namespace features {

namespace {

enum Which { kReShade = 0, kWindowed = 1, kRecorder = 2, kGpuLimit = 3, kLsConfig = 4, kSuperLog = 5, kCount = 6 };

const Info kInfo[kCount] = {
    { reshade::kId,
      "ReShade 输入直通",
      "在 Lossless Scaling 缩放游戏时，仍能使用 ReShade 覆盖层上的鼠标和键盘。",
      "Lossless Scaling 的窗口平时会截获鼠标和键盘，ReShade 菜单因此点不了、也输不了字。开启本功能后，用下方的快捷键即可切换直通："
      "直通开启时，你的输入会送往 ReShade 覆盖层；游戏或 Lossless Scaling 失去焦点时，直通会自动关闭。\n可随时开启或关闭。" },
    { windowed::kId,
      "窗口化模式与第二显示器",
      "让 Lossless Scaling 也能处理窗口化的游戏，或配合第二显示器使用。",
      "添加一块与游戏窗口同尺寸的虚拟显示器，让 Lossless Scaling 得以缩放窗口，而不必占用全屏。也可以只覆盖该窗口的一半（分屏），"
      "或位于其旁边（用作第二显示器）。\n本功能必须在 Lossless Scaling 启动前就位，因此开启后需要重启 Lossless Scaling 才生效；关闭则立即生效。" },
    { recorder::kId,
      "录制",
      "保留 Lossless Scaling 捕获画面的最后几秒，按快捷键即可保存。",
      "把游戏最近几秒的帧（即 Lossless Scaling 捕获到的画面）无损压缩后存放在内存里，按 Ctrl+Shift+F1 即可存为 .lsrec 文件："
      "适合可离线回放的缺陷报告、测试和前后对比。它会自行寻找帧：帧生成开启且未启用任何插件时，记录的是帧生成的输出"
      "（帧生成关闭时，取 Lossless Scaling 缩放处理趟所读取的那一帧）；插件则只补充仅它们才有的那部分帧。可随时开启或关闭。" },
    { gpulimit::kId,
      "GPU 限制",
      "把显卡负载控制在 80–100% 的上限内，供能降低自身开销的插件使用。",
      "插件管理器会读取运行 Lossless Scaling 的那块显卡的负载。负载连续几秒达到或超过上限时，插件就会降低自身开销："
      "神经渲染的自动画质会缩小模型的工作尺寸，各上采样器也会启用\"显卡吃满时用更轻的上采样\"；负载明显低于上限后，一切恢复原状。"
      "所有插件共用这一项设置。可随时开启或关闭。" },
    { lsconfig::kId,
      "Lossless Scaling 的隐藏设置",
      "调整其帧率计数器的外观与位置，以及它的窗口没有露出的几项专家数值。",
      "直接编辑 Lossless Scaling 自己的 config.ini（位于本程序旁边）：包括计数器的大小、颜色、字体和位置，以及若干专家数值。Lossless Scaling 在缩放开始时读取该文件，因此改动从下一次缩放起生效。只改动你修改过的行；首次修改时会先把原文件备份为 config.ini.bak-manager。" },
    { superlog::kId,
      "超级日志",
      "排查问题所需的一切，一键可得：一份详细日志，外加按快捷键即生成的报告。",
      "开启后日志等级为 Debug，按 Ctrl+Shift+F12（或对应按钮）即可保存报告：在 Lossless Scaling 旁的 reports 文件夹里生成一个 zip，"
      "内含日志、设置、系统信息（显卡、驱动、显示器、版本）、Lossless Scaling 窗口发送的内容与时间、各插件发布的每条序列最近十分钟的数据，以及每个插件的帧跟踪记录。"
      "插件报告错误时，它也会自动保存一份。任何内容都不会上传：文件由你自己发送。可随时开启或关闭。" },
};

ConfigManager& Cfg() { return ConfigManager::Instance(); }

} // namespace

int Count() { return kCount; }

const Info& At(int index) { return kInfo[index]; }

bool IsOn(int index) { return Cfg().IsAddonEnabled(kInfo[index].id, false); }

void SetOn(int index, bool on) {
    Cfg().SetAddonEnabled(kInfo[index].id, on);
    Cfg().Save();
    LOG_INFO("Features", "%s is now %s", kInfo[index].title, on ? "on" : "off");

    switch (index) {
        case kReShade:
            if (on) reshade::Start(); else reshade::Stop();
            break;
        case kWindowed:
            if (windowed::Started()) windowed::SetActive(on);   // hooks are in: the switch is live. Otherwise it waits for a restart.
            break;
        case kRecorder:
            if (on) recorder::Start(); else recorder::Stop();
            break;
        case kGpuLimit:
            if (on) gpulimit::Start(); else gpulimit::Stop();
            break;
        case kSuperLog:
            if (on) superlog::Start(); else superlog::Stop();
            break;
    }
}

bool NeedsRestart(int index) { return index == kWindowed && IsOn(index) && !windowed::Started(); }

std::string Status(int index) {
    switch (index) {
        case kReShade: return IsOn(index) ? reshade::Status() : std::string();
        case kWindowed: return IsOn(index) ? windowed::Status() : std::string();
        case kRecorder: return IsOn(index) ? recorder::Status() : std::string();
        case kGpuLimit: return IsOn(index) ? gpulimit::Status() : std::string();
        case kLsConfig: return {};
        case kSuperLog: return IsOn(index) ? superlog::Status() : std::string();
    }
    return {};
}

void RenderOptions(int index) {
    switch (index) {
        case kReShade: reshade::RenderOptions(); break;
        case kWindowed: windowed::RenderOptions(); break;
        case kRecorder: recorder::RenderOptions(); break;
        case kGpuLimit: gpulimit::RenderOptions(); break;
        case kLsConfig: lsconfig::RenderOptions(); break;
        case kSuperLog: superlog::RenderOptions(); break;
    }
}

// Up to 0.7.4 the features kept their settings under the ids of the standalone addons they came from; move them once.
void MoveOldSettings() {
    const char* const oldIds[kCount] = { reshade::kOldId, windowed::kOldId, recorder::kId, gpulimit::kId, lsconfig::kId, superlog::kId };   // (the recorder is new: its old id is its own, which renames nothing)
    bool moved = false;
    for (int i = 0; i < kCount; ++i)
        if (Cfg().RenameAddonSection(oldIds[i], kInfo[i].id)) { moved = true; LOG_INFO("Features", "%s: settings moved from %s to %s", kInfo[i].title, oldIds[i], kInfo[i].id); }
    if (moved) Cfg().Save();
}

void Start() {
    MoveOldSettings();
    if (IsOn(kReShade)) reshade::Start();
    if (IsOn(kWindowed)) windowed::Start();
    if (IsOn(kRecorder)) recorder::Start();
    if (IsOn(kGpuLimit)) gpulimit::Start();
    if (IsOn(kSuperLog)) superlog::Start();
}

void Stop() {
    superlog::Stop();
    gpulimit::Stop();
    recorder::Stop();
    reshade::Stop();
    windowed::Stop();
}

bool IsRetiredAddonId(const std::string& folderName) {
    return folderName == reshade::kOldId || folderName == windowed::kOldId;
}

} // namespace features
} // namespace eam
