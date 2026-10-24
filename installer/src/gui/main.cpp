// LS Addon Manager Setup: a small wizard over the installer core (installer.h), and a silent mode for scripts and tests.
//
//   LSAddonManagerSetup.exe                                         the wizard
//   LSAddonManagerSetup.exe --folder <Lossless Scaling folder>      the wizard, starting with that folder
//   LSAddonManagerSetup.exe --silent status|install|uninstall --folder <dir> [--remove-addons] [--payload <dir>] [--log <file>]
//   LSAddonManagerSetup.exe --version                               what this setup carries (used by the build to check it)
//   LSAddonManagerSetup.exe --folder <dir> --update-when-closed [--restart]
//                                                                   the manager's update: waits (a window with Cancel) until Lossless Scaling
//                                                                   running from that folder has closed, then updates it as the wizard does;
//                                                                   --restart starts Lossless Scaling again afterwards
//                                                                   KEEP THESE FLAGS: every released manager from 0.9.7 on starts a
//                                                                   newer Setup with exactly them (manager/src/update/updater.cpp)
//   LSAddonManagerSetup.exe --shot <dir> --folder <fake folder>     the README's pictures: the start page, then (after installing into that folder)
//                                                                   the result page, saved as setup-start.bmp and setup-done.bmp; the window
//                                                                   is kept off the screen, nothing is remembered. For a throwaway folder only.
//
// The files it installs are a resource of this exe (see payload.h); --payload <dir> uses a folder instead (for development and tests).
// The wizard is Windows' own TaskDialog: a few pages moved between with TDM_NAVIGATE_PAGE. Nothing here decides anything about files: that is the core's job.
#include "fileinfo.h"
#include "installer.h"
#include "lsfolder.h"
#include "payload.h"
#include "state.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;
using namespace setup;

namespace {

// --------------------------------------------------------------------------------------------------------------------------------------- words

const wchar_t* const kNoModelNotice =
    L"DLSS 5 神经渲染需要你自备的 nvngx_dlssnr.dll。它不在这份下载里，本项目既不会下载它，也不会提供获取途径。 "
    L"安装程序可以在之后把你的文件复制进文件夹。没有它，其余功能照常可用。\n"
    L"已在 World of Warcraft: Forever、Fallout: New Vegas 和 Lossless Scaling 3.2.2.0 上测试。与本项目所有文件一样，未经签名。";

std::wstring W(const std::string& s) { return Widen(s); }

std::wstring PlanText(Action a) {
    const wchar_t* safety =
        L"\n\n所有会被替换的文件都先复制到 Lossless Scaling 文件夹内的 backups 备份文件夹；不会删除任何东西。 "
        L"你的设置（addons\\config.json）、其他插件以及已移除的插件都绝不会被动到。复制完成后会校验新文件， "
        L"一旦出错，文件夹会恢复原样。";
    switch (a) {
    case Action::Install:
        return std::wstring(L"Lossless Scaling 自己的 Lossless.dll 会被保留，改名为 Lossless_original.dll（管理器把一切调用转交给它）。 "
                            L"LS 插件管理器的 Lossless.dll 取代它的位置，同时装入两个图标文件和 addons 文件夹。") + safety;
    case Action::Update:
        return std::wstring(L"先前的 LS 插件管理器文件会被这批文件替换；Lossless_original.dll 保持不变。") + safety;
    case Action::Repair:
        return std::wstring(L"Lossless Scaling 自我更新时，用它自己的 Lossless.dll 覆盖了 LS 插件管理器的版本。安装程序会把新文件保留为 Lossless_original.dll， "
                            L"并重新装回 LS 插件管理器的 Lossless.dll。") + safety;
    case Action::Reinstall:
        return std::wstring(L"把 LS 插件管理器的文件恢复成本次下载中的原始文件。") + safety;
    default: return L"";
    }
}

// --------------------------------------------------------------------------------------------------------------------------------------- state

enum Cmd {
    kAction = 1001, kUninstall, kUninstallAll, kOther, kAdmin, kRecheck, kPlaceModel, kOpenFolder, kOpenBackups, kBrowse, kBack, kToStart,
    kCandidate0 = 2000,
};

struct Page {
    std::wstring title, instruction, content, expanded, footer;
    std::vector<std::pair<int, std::wstring>> links;
    std::vector<TASKDIALOG_BUTTON> buttons;
    TASKDIALOGCONFIG cfg{};
    bool progress = false;
};

struct App {
    std::wstring exeDir, tempDir, payloadDir, folder, note;
    PayloadInfo payload;
    std::string payloadError;
    State state;
    Advice advice;
    std::vector<Candidate> candidates;
    bool elevated = false, writable = true;

    Result result;
    bool resultIsUninstall = false;
    std::thread worker;
    std::atomic<bool> done{false};
    bool showingProgress = false;

    bool waitForClose = false, restartAfter = false;   // --update-when-closed, --restart
    bool waiting = false;                              // the waiting page is shown
    bool restarted = false;
    DWORD lastLook = 0;

    int testCloseMs = 0;
    DWORD startTick = 0;
    std::wstring shotDir;   // --shot: where the two pictures go
    int shotStage = 0;      // 0 the start page, 1 installing, 2 the result page shown, 3 done
    DWORD shotTick = 0;
    HICON icon = nullptr;
    std::vector<std::unique_ptr<Page>> pages;   // kept alive until the dialog ends: the dialog may still point into the last ones
};

bool IsElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n)) r = e.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return r;
}

// Can this program create a file in the folder? (Program Files and some Steam libraries say no without administrator rights.)
bool CanWriteFolder(const std::wstring& dir) {
    const std::wstring probe = JoinPath(dir, L".echo_setup_write_test_" + std::to_wstring(GetCurrentProcessId()));
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

void Refresh(App& a) {
    a.state = Inspect(a.folder);
    a.advice = Advise(a.state, a.payload.ok ? a.payload.version : std::string());
    a.writable = CanWriteFolder(a.folder);
}

// A folder the person chose (from the list or with Browse) is remembered at once, so the next start offers it first, even if nothing was installed. Only a folder
// that really is Lossless Scaling's is kept.
void RememberChoice(const App& a) {
    if (a.state.situation != Situation::NotLosslessScaling) RememberFolder(a.folder);
}

// --------------------------------------------------------------------------------------------------------------------------------------- pages

HRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR data);

Page& NewPage(App& a, const std::wstring& instruction, const std::wstring& content, bool closeButton = true) {
    a.pages.push_back(std::make_unique<Page>());
    Page& p = *a.pages.back();
    p.title = L"LS 插件管理器 " + (a.payload.ok ? W(a.payload.version) + L" " : std::wstring()) + L"安装程序";
    p.instruction = instruction;
    p.content = content;
    p.footer = kNoModelNotice;
    p.cfg.cbSize = sizeof(p.cfg);
    p.cfg.pszWindowTitle = p.title.c_str();
    p.cfg.pszMainInstruction = p.instruction.c_str();
    p.cfg.pszContent = p.content.c_str();
    p.cfg.pfCallback = DialogProc;
    p.cfg.lpCallbackData = reinterpret_cast<LONG_PTR>(&a);
    p.cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_CALLBACK_TIMER;
    p.cfg.cxWidth = 260;
    p.cfg.dwCommonButtons = closeButton ? TDCBF_CLOSE_BUTTON : 0;
    if (a.icon) { p.cfg.dwFlags |= TDF_USE_HICON_MAIN; p.cfg.hMainIcon = a.icon; }
    else p.cfg.pszMainIcon = TD_INFORMATION_ICON;
    return p;
}

void Finish(Page& p, bool withFooter = true) {
    p.buttons.clear();
    for (const auto& l : p.links) p.buttons.push_back({l.first, l.second.c_str()});
    p.cfg.pButtons = p.buttons.empty() ? nullptr : p.buttons.data();
    p.cfg.cButtons = static_cast<UINT>(p.buttons.size());
    if (p.buttons.empty()) p.cfg.dwFlags &= ~static_cast<TASKDIALOG_FLAGS>(TDF_USE_COMMAND_LINKS);   // the flag without any custom button is E_INVALIDARG
    if (!p.expanded.empty()) {
        p.cfg.pszExpandedInformation = p.expanded.c_str();
        p.cfg.pszCollapsedControlText = L"显示详情";
        p.cfg.pszExpandedControlText = L"收起详情";
    }
    if (withFooter) {
        p.cfg.pszFooter = p.footer.c_str();
        p.cfg.pszFooterIcon = TD_INFORMATION_ICON;
    }
}

void Navigate(HWND hwnd, Page& p) { SendMessageW(hwnd, TDM_NAVIGATE_PAGE, 0, reinterpret_cast<LPARAM>(&p.cfg)); }

Page& MainPage(App& a) {
    a.showingProgress = false;
    if (!a.payload.ok) {
        Page& p = NewPage(a, L"这份安装程序文件已损坏。", W(a.payloadError.empty() ? "没有可安装的文件。" : a.payloadError) +
                                                            L"\n\n请从项目的发布页重新下载。");
        Finish(p, false);
        return p;
    }
    std::wstring content = W(a.advice.detail);
    if (!content.empty()) content += L"\n\n";
    content += L"Lossless Scaling 文件夹：\n" + a.folder;
    if (!a.state.lsVersion.empty()) content += L"\nLossless Scaling " + W(a.state.lsVersion);
    if (!a.note.empty()) { content += L"\n\n" + a.note; a.note.clear(); }
    if (a.advice.blocked) content += L"\n\n" + W(a.advice.blockedReason);
    const bool canChange = a.writable || a.elevated;
    if (!canChange && a.state.situation != Situation::NotLosslessScaling) content += L"\n\nWindows 只允许管理员修改该文件夹中的文件。";

    Page& p = NewPage(a, W(a.advice.headline), content);
    const bool haveAction = a.advice.action != Action::None && !a.advice.blocked;
    if (haveAction && canChange) {
        std::wstring label = W(a.advice.actionLabel);
        if (a.advice.action == Action::Install) label += L"\n保留 Lossless Scaling 自己的文件，安装管理器及其插件";
        else if (a.advice.action == Action::Update) label += L"\n替换旧版本";
        else if (a.advice.action == Action::Repair) label += L"\n在 Lossless Scaling 更新后装回管理器";
        else if (a.advice.action == Action::Reinstall) label += L"\n恢复为原始文件（不含你修改过的）";
        p.links.push_back({kAction, label});
        p.expanded = PlanText(a.advice.action);
    }
    if ((haveAction || a.advice.canUninstall) && !canChange) p.links.push_back({kAdmin, L"以管理员身份重启\n此文件夹只能用管理员权限修改"});
    if (a.advice.blocked) p.links.push_back({kRecheck, L"重新检查\n请先关闭 Lossless Scaling"});
    if (a.advice.canUninstall && canChange && !a.advice.blocked) {
        p.links.push_back({kUninstall, L"卸载\n装回 Lossless Scaling 自己的 Lossless.dll。你的插件和设置保持不变。"});
        p.links.push_back({kUninstallAll, L"卸载并移除插件\naddons 文件夹连同各插件的设置会移入备份文件夹，不会被删除。"});
    }
    p.links.push_back({kOther, L"使用其他文件夹..."});
    Finish(p);
    return p;
}

Page& ChooserPage(App& a, const std::wstring& why) {
    a.showingProgress = false;
    std::wstring content = why;
    if (!content.empty()) content += L"\n\n";
    content += a.candidates.empty() ? L"安装程序未能自行找到 Lossless Scaling。请选择它的文件夹：即包含 LosslessScaling.exe 的那个。"
                                    : L"请选择你要使用的那个 Lossless Scaling 的文件夹。安装程序列出了找到的结果；点“浏览”可选其他任意文件夹。";
    Page& p = NewPage(a, L"选择哪个 Lossless Scaling？", content);
    int i = 0;
    for (const auto& c : a.candidates) {
        if (i >= 8) break;
        const State s = Inspect(c.dir);
        std::wstring line = c.dir + L"\n";
        line += s.installedVersion.empty() ? L"此处未安装 LS 插件管理器" : L"此处已安装 " + W(s.installedVersion) + L" 版本的 LS 插件管理器";
        if (!s.lsVersion.empty()) line += L"  |  Lossless Scaling " + W(s.lsVersion);
        line += L"  |  来源：" + W(c.how);
        p.links.push_back({kCandidate0 + i, line});
        ++i;
    }
    p.links.push_back({kBrowse, L"浏览文件夹..."});
    if (!a.folder.empty()) p.links.push_back({kBack, L"返回"});
    Finish(p);
    return p;
}

Page& ProgressPage(App& a, const std::wstring& what) {
    a.showingProgress = true;
    Page& p = NewPage(a, what, L"这只需要几秒钟。请不要关机或断开电源。", false);
    p.progress = true;
    p.cfg.dwFlags |= TDF_SHOW_MARQUEE_PROGRESS_BAR;
    Finish(p, false);
    return p;
}

// The manager's update: Lossless Scaling has to close first (the manager lives inside it). Checked a few times a second.
Page& WaitingPage(App& a) {
    a.showingProgress = true;   // the marquee; and the window test does not close it while it waits
    a.waiting = true;
    Page& p = NewPage(a, L"关闭 Lossless Scaling，以更新 LS 插件管理器" + (a.payload.ok ? L"到版本 " + W(a.payload.version) : std::wstring()) + L"。",
                      L"安装程序正在等待。请关闭 Lossless Scaling：直接关掉它的窗口，或右键点击时钟旁的图标并选择“退出”。\n\n"
                      L"随后更新会自动进行，并照常创建备份" +
                      std::wstring(a.restartAfter ? L"，之后 Lossless Scaling 会重新启动。" : L"。") +
                      L"\n\nLossless Scaling 文件夹：\n" + a.folder);
    p.progress = true;
    p.cfg.dwFlags |= TDF_SHOW_MARQUEE_PROGRESS_BAR;
    Finish(p, false);
    return p;
}

// Lossless Scaling again, after the manager's update (--restart). From an elevated Setup it is started through Explorer, so that it does not
// inherit administrator rights it never had.
void RestartLosslessScaling(App& a) {
    const std::wstring exe = JoinPath(a.folder, L"LosslessScaling.exe");
    if (!Exists(exe)) return;
    if (a.elevated) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + exe + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    else ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, a.folder.c_str(), SW_SHOWNORMAL);
    a.restarted = true;
}

Page& ResultPage(App& a) {
    a.showingProgress = false;
    const Result& r = a.result;
    std::wstring instruction;
    if (r.ok) instruction = a.resultIsUninstall ? L"已卸载。" : L"完成。";
    else instruction = r.rolledBack ? L"未能完成，未更改任何文件。" : L"未能完成。";
    std::wstring content = W(r.message);
    if (r.ok && !a.resultIsUninstall) content += a.restarted ? L"\n\nLossless Scaling 正在重新启动；管理器会随之打开。"
                                                             : L"\n\n请启动一次 Lossless Scaling；管理器会自行打开。";
    if (!a.note.empty()) { content += L"\n\n" + a.note; a.note.clear(); }
    Page& p = NewPage(a, instruction, content);
    if (!r.log.empty() || !r.backupDir.empty()) {
        for (const auto& line : r.log) p.expanded += W(line) + L"\n";
        if (!r.backupDir.empty()) p.expanded += L"\n备份文件夹：" + r.backupDir;
    }
    if (r.ok && !a.resultIsUninstall && !Exists(JoinPath(a.folder, L"nvngx_dlssnr.dll")))
        p.links.push_back({kPlaceModel, L"把我的 nvngx_dlssnr.dll 复制到此文件夹...\n仅用于 DLSS 5 神经渲染。选择你已有的文件即可；不会下载任何东西。"});
    p.links.push_back({kOpenFolder, L"打开 Lossless Scaling 文件夹"});
    if (!r.backupDir.empty()) p.links.push_back({kOpenBackups, L"打开备份文件夹\n被替换或移除的文件都在里面"});
    p.links.push_back({kToStart, L"返回开始页"});
    Finish(p);
    return p;
}

// --------------------------------------------------------------------------------------------------------------------------------------- actions

// A folder picker (FOS_PICKFOLDERS) or a file picker for a .dll; returns an empty string when cancelled.
std::wstring PickFile(HWND owner, bool folders) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    if (folders) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"选择 Lossless Scaling 文件夹（即包含 LosslessScaling.exe 的那个）");
    } else {
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        dlg->SetTitle(L"选择你的 nvngx_dlssnr.dll");
        const COMDLG_FILTERSPEC filter[] = {{L"DLL 文件", L"*.dll"}, {L"所有文件", L"*.*"}};
        dlg->SetFileTypes(2, filter);
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { result = path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

// Copy the person's own model file next to LosslessScaling.exe, checked by hash; only when there is none yet, so nothing is overwritten.
std::wstring PlaceModel(App& a, const std::wstring& src) {
    const std::wstring target = JoinPath(a.folder, L"nvngx_dlssnr.dll");
    if (Exists(target)) return L"该文件夹中已有 nvngx_dlssnr.dll，因此没有复制。";
    uint64_t size = 0;
    if (!FileSizeOf(src, size) || size == 0) return L"无法读取该文件，或文件是空的。";
    const std::wstring temp = target + L".tmp";
    if (!CopyFileW(src.c_str(), temp.c_str(), FALSE)) return L"无法把文件复制到该文件夹。";
    if (Sha256File(src).empty() || Sha256File(src) != Sha256File(temp)) { DeleteFileW(temp.c_str()); return L"副本与原文件不一致，已将其删除。"; }
    if (!MoveFileExW(temp.c_str(), target.c_str(), 0)) { DeleteFileW(temp.c_str()); return L"无法把副本放到目标位置。"; }
    return L"已把 nvngx_dlssnr.dll 复制到文件夹。打开管理器，进入神经渲染面板，点击“测试兼容性”。";
}

void StartWork(HWND hwnd, App& a, bool uninstall, bool removeAddons) {
    a.done = false;
    a.resultIsUninstall = uninstall;
    Navigate(hwnd, ProgressPage(a, uninstall ? L"正在卸载 LS 插件管理器..." : L"正在安装 LS 插件管理器..."));
    if (a.worker.joinable()) a.worker.join();
    a.worker = std::thread([&a, uninstall, removeAddons] {
        a.result = uninstall ? Uninstall(a.folder, removeAddons) : Install(a.folder, a.payloadDir);
        a.done = true;
    });
}

bool RestartElevated(App& a) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // A folder that ends in a backslash (a drive root) would escape the closing quote in the command line: such a backslash is written twice.
    const std::wstring folderArg = (!a.folder.empty() && a.folder.back() == L'\\') ? a.folder + L"\\" : a.folder;
    const std::wstring args = L"--folder \"" + folderArg + L"\"" + (a.tempDir.empty() || a.payloadDir.empty() || a.payloadDir.find(a.tempDir) == 0 ? L"" : L" --payload \"" + a.payloadDir + L"\"");
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"runas", exe, args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

HRESULT OnButton(HWND hwnd, App& a, int id) {
    if (id >= kCandidate0 && id < kCandidate0 + 8) {
        const size_t i = static_cast<size_t>(id - kCandidate0);
        if (i < a.candidates.size()) { a.folder = a.candidates[i].dir; Refresh(a); RememberChoice(a); Navigate(hwnd, MainPage(a)); }
        return S_FALSE;
    }
    switch (id) {
    case kAction: StartWork(hwnd, a, false, false); return S_FALSE;
    case kUninstall: StartWork(hwnd, a, true, false); return S_FALSE;
    case kUninstallAll: StartWork(hwnd, a, true, true); return S_FALSE;
    case kOther: Navigate(hwnd, ChooserPage(a, L"")); return S_FALSE;
    case kBack: case kToStart: case kRecheck:
        a.candidates = FindCandidates();
        Refresh(a);
        Navigate(hwnd, MainPage(a));
        return S_FALSE;
    case kBrowse: {
        const std::wstring picked = PickFile(hwnd, true);
        if (!picked.empty()) {
            a.folder = picked;
            Refresh(a);
            if (a.state.situation == Situation::NotLosslessScaling) {   // the parent of the folder was picked: use the Lossless Scaling folder inside it, if there is exactly one
                const auto inside = ScanDrive(picked);
                if (inside.size() == 1) { a.folder = inside[0]; Refresh(a); a.note = L"Lossless Scaling 就在你选择的文件夹里面，因此安装程序改用它。"; }
            }
            RememberChoice(a);
            Navigate(hwnd, MainPage(a));
        }
        return S_FALSE;
    }
    case kAdmin:
        if (RestartElevated(a)) return S_OK;   // the elevated copy takes over; this one closes
        a.note = L"安装程序无法以管理员身份重启自身。";
        Navigate(hwnd, MainPage(a));
        return S_FALSE;
    case kPlaceModel: {
        const std::wstring src = PickFile(hwnd, false);
        if (!src.empty()) a.note = PlaceModel(a, src);
        Navigate(hwnd, ResultPage(a));
        return S_FALSE;
    }
    case kOpenFolder: ShellExecuteW(nullptr, L"open", a.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return S_FALSE;
    case kOpenBackups: ShellExecuteW(nullptr, L"open", a.result.backupDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return S_FALSE;
    default: return S_OK;   // Close
    }
}

// The window as it is drawn, into a 32-bit BMP (PrintWindow with the full content, so it works off the screen too).
bool SaveWindowPicture(HWND hwnd, const std::wstring& path) {
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return false;
    const int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    const bool drawn = PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT) != FALSE;
    bool ok = false;
    if (drawn && bits) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
            BITMAPFILEHEADER fh{}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
            fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w) * h * 4;
            BITMAPINFOHEADER ih = bi.bmiHeader; ih.biHeight = -h;   // top-down rows, as drawn
            ok = fwrite(&fh, sizeof fh, 1, f) == 1 && fwrite(&ih, sizeof ih, 1, f) == 1 && fwrite(bits, static_cast<size_t>(w) * h * 4, 1, f) == 1;
            fclose(f);
        }
    }
    SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); ReleaseDC(nullptr, screen);
    return ok;
}

HRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LONG_PTR data) {
    App& a = *reinterpret_cast<App*>(data);
    switch (msg) {
    case TDN_CREATED:
    case TDN_NAVIGATED:
        if (a.showingProgress) SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
        if (!a.shotDir.empty()) SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);   // off the screen
        break;
    case TDN_TIMER:
        if (!a.shotDir.empty()) {   // the README's pictures: the start page, install, the result page, close
            const DWORD now = GetTickCount();
            if (a.shotStage == 0 && now - a.startTick > 1200) {
                SaveWindowPicture(hwnd, JoinPath(a.shotDir, L"setup-start.bmp"));
                a.shotStage = 1;
                SendMessageW(hwnd, TDM_CLICK_BUTTON, kAction, 0);
            } else if (a.shotStage == 2 && now - a.shotTick > 1200) {
                SaveWindowPicture(hwnd, JoinPath(a.shotDir, L"setup-done.bmp"));
                a.shotStage = 3;
                SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCLOSE, 0);
            }
        }
        if (a.waiting && GetTickCount() - a.lastLook >= 200) {
            a.lastLook = GetTickCount();
            if (!LosslessScalingRunning(a.folder)) {
                a.waiting = false;
                Refresh(a);
                const bool canChange = a.writable || a.elevated;
                if (a.advice.action != Action::None && !a.advice.blocked && canChange && a.payload.ok) StartWork(hwnd, a, false, false);
                else Navigate(hwnd, MainPage(a));   // something else is in the way: the usual page says what, and what to do
            }
        } else if (a.showingProgress && a.done) {
            if (a.worker.joinable()) a.worker.join();
            if (a.result.ok && !a.resultIsUninstall && a.shotDir.empty()) RememberFolder(a.folder);
            Refresh(a);
            if (a.result.ok && !a.resultIsUninstall && a.restartAfter) RestartLosslessScaling(a);
            Navigate(hwnd, ResultPage(a));
            if (a.shotStage == 1) { a.shotStage = 2; a.shotTick = GetTickCount(); }
        } else if (a.testCloseMs > 0 && !a.showingProgress && GetTickCount() - a.startTick > static_cast<DWORD>(a.testCloseMs)) {
            SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCLOSE, 0);   // the window test closes the wizard by itself
        }
        break;
    case TDN_BUTTON_CLICKED:
        return OnButton(hwnd, a, static_cast<int>(wp));
    }
    return S_OK;
}

// --------------------------------------------------------------------------------------------------------------------------------------- command line

std::wstring Flag(const std::vector<std::wstring>& args, const wchar_t* name) {
    for (size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == name) return args[i + 1];
    return std::wstring();
}
bool Has(const std::vector<std::wstring>& args, const wchar_t* name) {
    for (const auto& a : args) if (a == name) return true;
    return false;
}

struct Out {
    FILE* file = nullptr;
    bool console = false;
    void Line(const std::string& s) {
        if (file) { fputs((s + "\n").c_str(), file); fflush(file); }
        if (console) puts(s.c_str());
    }
};

const char* SituationText(Situation s) {
    switch (s) {
    case Situation::NotLosslessScaling: return "not a Lossless Scaling folder";
    case Situation::NotInstalled: return "not installed";
    case Situation::Installed: return "installed";
    case Situation::AfterLsUpdate: return "after a Lossless Scaling update";
    case Situation::NoOriginal: return "no original";
    case Situation::BothOurs: return "both ours";
    case Situation::Unrecognised: return "unrecognised";
    }
    return "?";
}

int RunSilent(App& a, const std::vector<std::wstring>& args, Out& out) {
    const std::wstring what = Flag(args, L"--silent");
    if (a.folder.empty()) { out.Line("a folder is needed: --folder <Lossless Scaling folder>"); return 2; }
    Refresh(a);
    if (what == L"status") {
        out.Line(std::string("situation: ") + SituationText(a.state.situation));
        out.Line("installed: " + (a.state.installedVersion.empty() ? std::string("-") : a.state.installedVersion));
        out.Line("payload: " + (a.payload.ok ? a.payload.version : std::string("none")));
        out.Line("advice: " + a.advice.headline);
        return 0;
    }
    if (what != L"install" && what != L"uninstall") { out.Line("usage: --silent status|install|uninstall"); return 2; }
    if (what == L"install" && !a.payload.ok) { out.Line("no files to install: " + (a.payloadError.empty() ? std::string("this setup carries none") : a.payloadError)); return 2; }
    const Result r = what == L"install" ? Install(a.folder, a.payloadDir) : Uninstall(a.folder, Has(args, L"--remove-addons"));
    for (const auto& l : r.log) out.Line("  " + l);
    out.Line(r.message);
    if (!r.backupDir.empty()) out.Line("backups: " + Narrow(r.backupDir));
    if (r.ok && what == L"install") RememberFolder(a.folder);
    return r.ok ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    wchar_t** argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argvW, argvW + argc);
    LocalFree(argvW);

    App a;
    a.elevated = IsElevated();
    a.startTick = GetTickCount();
    a.icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    const bool silent = Has(args, L"--silent"), showVersion = Has(args, L"--version");
    // --extract <folder>: only write the files this Setup carries into that folder (new or empty), for installing by hand; nothing else is touched
    const std::wstring extract = Flag(args, L"--extract");
    const std::wstring testClose = Flag(args, L"--test-close-ms");
    if (!testClose.empty()) a.testCloseMs = _wtoi(testClose.c_str());
    a.shotDir = Flag(args, L"--shot");
    if (!testClose.empty() || !a.shotDir.empty() || Has(args, L"--no-remember"))
        UseRegistryKeyForTest(L"Software\\LSAddonManager\\SetupTest");   // tests must never touch the person's remembered folder

    Out out;
    const std::wstring logPath = Flag(args, L"--log");
    if (!logPath.empty()) _wfopen_s(&out.file, logPath.c_str(), L"wb");
    if ((silent || showVersion || !extract.empty()) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        out.console = true;
    }

    // The files to install: a folder given on the command line, or the bundle inside this exe (unpacked to a temporary folder)
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring given = Flag(args, L"--payload");
    if (!given.empty()) {
        a.payloadDir = given;
    } else {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!EmbeddedPayload(data, size)) {
            a.payloadError = "这份安装程序没有携带可安装的文件（构建时未打包）。";
        } else {
            a.tempDir = JoinPath(tmp, L"LSAddonManagerSetup-" + std::to_wstring(GetCurrentProcessId()));
            std::string err;
            if (UnpackTo(data, size, JoinPath(a.tempDir, L"files"), err)) a.payloadDir = JoinPath(a.tempDir, L"files");
            else a.payloadError = err;
        }
    }
    if (!a.payloadDir.empty()) {
        a.payload = CheckPayload(a.payloadDir);
        if (!a.payload.ok) a.payloadError = a.payload.error;
    }

    int code = 0;
    if (!extract.empty()) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        std::string err;
        const DWORD attr = GetFileAttributesW(extract.c_str());
        bool emptyOrNew = attr == INVALID_FILE_ATTRIBUTES;
        if (!emptyOrNew && (attr & FILE_ATTRIBUTE_DIRECTORY)) {   // a folder with nothing in it but . and ..
            WIN32_FIND_DATAW fd{}; emptyOrNew = true;
            const HANDLE h = FindFirstFileW((extract + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) emptyOrNew = false; } while (emptyOrNew && FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        if (!a.payload.ok) { out.Line("nothing to extract: " + a.payloadError); code = 2; }
        else if (!emptyOrNew) { out.Line("--extract needs a new or empty folder"); code = 2; }
        else if (!EmbeddedPayload(data, size) || !UnpackTo(data, size, extract, err)) { out.Line("could not extract: " + err); code = 3; }
        else out.Line("extracted LS Addon Manager " + a.payload.version + "'s files; copy them into the Lossless Scaling folder (see INSTALL.txt)");
    } else if (showVersion) {
        out.Line(a.payload.ok ? "payload " + a.payload.version : "payload none: " + a.payloadError);
        code = a.payload.ok ? 0 : 2;
    } else if (silent) {
        a.folder = Flag(args, L"--folder");
        code = RunSilent(a, args, out);
    } else if (HANDLE once = CreateMutexW(nullptr, TRUE, (L"Local\\LSAddonManagerSetup" + Flag(args, L"--instance-name")).c_str()); GetLastError() == ERROR_ALREADY_EXISTS) {
        // A second window over the same folder could start a second install in the middle of the first: one Setup window at a time.
        if (a.testCloseMs == 0) MessageBoxW(nullptr, L"LS 插件管理器安装程序已经打开。请在任务栏上查找它的窗口。", L"LS 插件管理器安装程序", MB_OK | MB_ICONINFORMATION);
        code = 4;
        if (once) CloseHandle(once);
    } else {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        a.candidates = FindCandidates();
        a.folder = Flag(args, L"--folder");
        a.waitForClose = Has(args, L"--update-when-closed");
        a.restartAfter = Has(args, L"--restart");
        Page* first = nullptr;
        if (a.waitForClose && !a.folder.empty()) {   // the manager's update: the waiting page (it moves on at once if Lossless Scaling is not running)
            Refresh(a);
            first = &WaitingPage(a);
        } else if (!a.folder.empty() || a.candidates.size() == 1) {
            if (a.folder.empty()) a.folder = a.candidates[0].dir;
            Refresh(a);
            first = &MainPage(a);
        } else {
            first = &ChooserPage(a, L"");
        }
        int button = 0;
        const HRESULT hr = TaskDialogIndirect(&first->cfg, &button, nullptr, nullptr);
        if (FAILED(hr)) { code = 3; out.Line("the window could not be created: 0x" + std::to_string(static_cast<unsigned long>(hr))); }
        if (a.worker.joinable()) a.worker.join();
        CoUninitialize();
        if (once) { ReleaseMutex(once); CloseHandle(once); }
    }
    if (out.file) fclose(out.file);
    if (!a.tempDir.empty()) { std::error_code ec; fs::remove_all(a.tempDir, ec); }
    return code;
}
