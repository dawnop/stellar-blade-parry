// SBParry — 剑星完美弹反/闪避练习工具 / Stellar Blade parry & dodge timing trainer
//
// 结构：
//   game.cpp     附加游戏进程、安装/还原钩子（hooks.asm）
//   tracker.cpp  步骤表 -> 音符预测；事件日志 -> 完美判定、蓝紫光判定；掉血记录；一击必杀
//   projectiles.cpp 飞行道具实时追踪
//   live.cpp     玩家/敌人/相机、血条、可惩戒状态、过场 QTE
//   autoplay.cpp 自动弹反 / 闪避 / 跳跃 / 惩戒 / 连打
//   overlay.cpp  绘制
//   main.cpp     主循环、快捷键、托盘
#include "autoplay.h"
#include "config.h"
#include "game.h"
#include "live.h"
#include "overlay.h"
#include "tracker.h"
#include <dwmapi.h>
#include <shellapi.h>

double g_tscPerSec = 3.0e9;

static const double kHorizon = 1.0; // 判定条显示未来多少秒
static const UINT WM_TRAY = WM_APP + 1;
static UINT g_wmTaskbarCreated; // 资源管理器重启后托盘图标要重新加

// 快捷键
enum HotkeyId { HK_BAR = 1, HK_MODE, HK_PANEL, HK_CORNER, HK_AUTO, HK_LANG, HK_QUIT, HK_LOG, HK_KILL, HK_QTE };
static const struct { int id; UINT vk; } kHotkeys[] = {
    {HK_BAR, 'L'}, {HK_MODE, 'M'}, {HK_PANEL, 'P'}, {HK_CORNER, 'O'}, {HK_AUTO, 'A'}, {HK_KILL, 'K'}, {HK_QTE, 'X'}, {HK_LANG, 'J'}, {HK_QUIT, 'Q'},
};

static std::wstring g_toast;
static ULONGLONG g_toastAt;

// 屏幕上按界面语言显示，日志里记英文：表达式在 EnglishScope 里再求一次值
static void ShowToast(const std::wstring& s, const std::wstring& en) {
    g_toast = s;
    g_toastAt = GetTickCount64();
    Log(L"[SBParry] %s\n", en.c_str());
}
#define Toast(expr) ShowToast(expr, [&] { EnglishScope en_; return std::wstring(expr); }())

static const wchar_t* OnOff(bool b) { return b ? TR("开", "ON") : TR("关", "OFF"); }

// 用系统默认的文本编辑器打开日志文件（程序目录下的 sbparry.log）
static void OpenLogFile() {
    wchar_t path[MAX_PATH];
    GetFullPathNameW(L"sbparry.log", MAX_PATH, path, nullptr);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) Log(L"SBParry log\n"); // 还没有就先建一个
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(nullptr, L"open", L"notepad.exe", path, nullptr, SW_SHOWNORMAL);
}

static void DoCommand(int id) {
    switch (id) {
    case HK_BAR:
        g_cfg.barVisible = !g_cfg.barVisible;
        Toast(std::wstring(TR("判定条：", "Timing bar: ")) + OnOff(g_cfg.barVisible));
        break;
    case HK_MODE:
        g_cfg.barMode ^= 1;
        Toast(g_cfg.barMode ? TR("判定条位置：跟随敌人", "Bar position: follow enemy") : TR("判定条位置：血条下方", "Bar position: under boss HP"));
        break;
    case HK_PANEL:
        g_cfg.panelVisible = !g_cfg.panelVisible;
        Toast(std::wstring(TR("统计面板：", "Stats panel: ")) + OnOff(g_cfg.panelVisible));
        break;
    case HK_CORNER: g_cfg.panelCorner = (g_cfg.panelCorner + 1) & 3; break;
    case HK_AUTO:
        g_cfg.autoParry = !g_cfg.autoParry;
        Toast(g_cfg.autoParry ? std::wstring(TR("自动弹反：开（", "Auto parry: ON (")) + AutoDeviceName() + TR("）", ")")
                              : std::wstring(TR("自动弹反：关", "Auto parry: OFF")));
        break;
    case HK_QTE:
        g_cfg.autoQte = !g_cfg.autoQte;
        Toast(std::wstring(TR("自动惩戒 / QTE：", "Auto finisher / QTE: ")) + OnOff(g_cfg.autoQte));
        break;
    case HK_KILL:
        if (!StepsLoaded()) { Toast(TR("一击必杀：等游戏连上后再开", "One-hit kill: wait until attached")); return; }
        SetOneHitKill(!OneHitKillOn());
        Toast(std::wstring(TR("一击必杀：", "One-hit kill: ")) + OnOff(OneHitKillOn()));
        return; // 不存进 ini：每次启动默认关
    case HK_LANG:
        g_cfg.language = g_lang == Lang::Zh ? 2 : 1;
        SaveConfig();
        Toast(g_lang == Lang::Zh ? TR("语言：中文", "Language: Chinese") : TR("语言：English", "Language: English"));
        break;
    case HK_LOG: OpenLogFile(); return;
    case HK_QUIT: Log(TR("[SBParry] 退出（快捷键/托盘）\n", "[SBParry] Quit (hotkey/tray)\n")); PostQuitMessage(0); return;
    }
    SaveConfig();
}

// ---------------------------------------------------------------- 托盘

static NOTIFYICONDATAW g_tray{};

static void TrayAdd(HWND h) {
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = h;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, L"SBParry " SBP_VERSION);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

static void TrayMenu(HWND h) {
    HMENU m = CreatePopupMenu();
    auto item = [&](int id, const wchar_t* text, bool checked, const wchar_t* key) {
        std::wstring s = std::wstring(text) + (key ? std::wstring(L"\tCtrl+Alt+") + key : L"");
        AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0), id, s.c_str());
    };
    item(HK_BAR, TR("显示判定条", "Show timing bar"), g_cfg.barVisible, L"L");
    item(HK_MODE, TR("判定条跟随敌人", "Bar follows enemy"), g_cfg.barMode == 1, L"M");
    item(HK_PANEL, TR("统计面板", "Stats panel"), g_cfg.panelVisible, L"P");
    item(HK_CORNER, TR("面板换角落", "Move panel corner"), false, L"O");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(HK_AUTO, TR("自动弹反", "Auto parry"), g_cfg.autoParry, L"A");
    item(HK_QTE, TR("自动惩戒 / QTE", "Auto finisher / QTE"), g_cfg.autoQte, L"X");
    item(HK_KILL, TR("一击必杀（跳过阶段）", "One-hit kill (skip phases)"), OneHitKillOn(), L"K");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(HK_LANG, L"中文 / English", false, L"J");
    item(HK_LOG, TR("打开日志文件", "Open log file"), false, nullptr);
    item(HK_QUIT, TR("退出（还原游戏代码）", "Quit (restore game code)"), false, L"Q");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
    DestroyMenu(m);
    if (cmd) DoCommand(cmd);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY: DoCommand((int)wp); return 0;
    case WM_INPUT: AutoOnRawInput(lp); break;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) TrayMenu(h);
        return 0;
    case WM_ENDSESSION:
        // 关机 / 注销：这条消息返回后进程随时会被结束，就地还原游戏代码
        if (wp) { AutoRelease(); SetOneHitKill(false); Detach(true); }
        return 0;
    }
    if (msg == g_wmTaskbarCreated && msg) { Shell_NotifyIconW(NIM_ADD, &g_tray); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- 状态与放置

// 连上游戏后的自检：没有 Bridge / 步骤表迟迟没导出时给出提示
static const wchar_t* StatusText() {
    static ULONGLONG attachedAt = 0;
    if (!g.ok) { attachedAt = 0; return nullptr; }
    if (!attachedAt) attachedAt = GetTickCount64();
    if (StepsLoaded()) return nullptr;
    if (GetTickCount64() - attachedAt < 8000) return nullptr;
    if (GetFileAttributesW(BridgeDir().c_str()) == INVALID_FILE_ATTRIBUTES)
        return TR("未找到 SBParryBridge：请把它放进 ue4ss\\Mods 并在 mods.txt 启用",
                  "SBParryBridge not found: copy it into ue4ss\\Mods and enable it in mods.txt");
    return TR("等待 SBParryBridge 导出步骤表…（UE4SS 是否已加载？）", "Waiting for SBParryBridge to export the step table… (is UE4SS loaded?)");
}

// 判定条位置：模式 0 固定在 boss 血条正下方；模式 1 跟随敌人头顶但限制在画面中央区域
// （血条区域按 2560x1440 截图量得：x 840~1720, y 80~150，按客户区比例缩放）
static bool PlaceBar(const RECT& r, int& x, int& y) {
    SIZE sz = OverlayBarSize();
    int W = r.right - r.left, H = r.bottom - r.top;
    if (g_cfg.barMode == 0) {
        x = r.left + W / 2 - sz.cx / 2;
        y = r.top + H * 105 / 1000;
        return true;
    }
    float loc[3];
    int sx, sy;
    if (!ActorLocation(g_live.enemy, loc)) return false;
    loc[2] += 200.f; // 胶囊体中心上方 2 米
    if (!Project(loc, sx, sy)) return false;
    // 区域比判定条还小（小窗口 + 大缩放）时贴着区域左/上边
    x = std::max<int>(r.left + W * 25 / 100, std::min<int>(sx - sz.cx / 2, r.left + W * 75 / 100 - sz.cx));
    y = std::max<int>(r.top + H * 13 / 100, std::min<int>(sy - sz.cy, r.top + H * 60 / 100 - sz.cy));
    return true;
}

static void Tick() {
    static ULONGLONG lastTry = 0, lastSteps = 0, lastPad = 0;
    ULONGLONG now = GetTickCount64();
    if (g.ok) {
        if (WaitForSingleObject(g.h, 0) == WAIT_OBJECT_0) {
            Log(TR("[SBParry] 游戏已退出\n", "[SBParry] Game exited\n"));
            Detach(false);
            ResetTracker();
        } else if (!Poll()) {
            Log(TR("[SBParry] 读取失败，重新连接\n", "[SBParry] Read failed, reconnecting\n"));
            SetOneHitKill(false); // 游戏还在：先把伤害倍率写回去、还原钩子，再断开
            Detach(true);
            ResetTracker();
        }
    } else if (now - lastTry > 2000) {
        lastTry = now;
        Attach();
    }
    if (g.ok && !StepsLoaded() && now - lastSteps > 3000) { lastSteps = now; LoadSteps(); }
    if (g.ok && now - lastPad > 1000) { lastPad = now; RefreshPadHooks(); }
    LiveTick();
    if (g.ok) UpdateNotes(TargetDistance());
    TrackHp(g.ok ? EveHpPercent() : -1); // 断开时把没记完的一次掉血记下来
    AutoTick();

    HudFrame f{};
    f.connected = g.ok;
    f.status = StatusText();
    f.window = g_window;
    f.horizon = kHorizon;
    f.notes = &g_notes;
    f.results = &g_results;
    f.autoOn = g_cfg.autoParry;
    f.targetDistance = TargetDistance();
    f.now = now / 1000.0;
    double toastAge = (now - g_toastAt) / 1000.0;
    if (!g_toast.empty() && toastAge < 2.0) { f.toast = g_toast.c_str(); f.toastAge = toastAge; }
    else if (f.status) { f.toast = f.status; f.toastAge = 0.5; } // 状态提示常驻

    RECT r{};
    bool client = g.ok && GameClient(r);
    // 切到别的程序时判定条和面板不要盖在它上面（窗口化 / 无边框时）
    bool fg = client && GameForeground();
    int x = 0, y = 0;
    bool bar = fg && g_cfg.barVisible && g_live.enemy && StepsLoaded() && PlaceBar(r, x, y);
    OverlayDrawBar(f, x, y, bar);
    if (!client) {
        // 没有游戏窗口：面板和提示放在主显示器工作区
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    }
    OverlayDrawPanel(f, r, g_cfg.panelCorner, g_cfg.panelVisible && (fg || !client));
    OverlayDrawToast(f, r);
}

// 本程序崩溃：尽量松开模拟按键、写回伤害倍率、还原钩子，别让游戏留在被改过的状态
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) {
        AutoRelease();
        SetOneHitKill(false);
        Detach(true);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void CalibrateTsc() {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    uint64_t t0 = __rdtsc();
    Sleep(200);
    QueryPerformanceCounter(&b);
    uint64_t t1 = __rdtsc();
    g_tscPerSec = (double)(t1 - t0) / ((double)(b.QuadPart - a.QuadPart) / f.QuadPart);
}

int wmain(int argc, wchar_t** argv) {
    // 工作目录固定为程序所在目录（ini / 校准 / 日志都放这里）
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    *wcsrchr(exe, L'\\') = 0;
    SetCurrentDirectoryW(exe);

    // 只允许一个实例；sbparry.exe --quit 让正在运行的实例还原游戏代码后退出
    HANDLE quitEvent = CreateEventW(nullptr, TRUE, FALSE, L"SBParry.Quit");
    for (int i = 1; i < argc; i++)
        if (!_wcsicmp(argv[i], L"--quit")) { SetEvent(quitEvent); return 0; }
    CreateMutexW(nullptr, TRUE, L"SBParry.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    ResetEvent(quitEvent);
    SetUnhandledExceptionFilter(CrashHandler);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // 与游戏同用物理像素
    LoadConfig();
    OpenTimingLog();
    CalibrateTsc();
    Log(L"SBParry " SBP_VERSION L" — %s\n", TR("剑星完美弹反/闪避练习工具", "Stellar Blade parry & dodge timing trainer"));
    Log(TR("  Ctrl+Alt+L 判定条  M 位置  P 面板  O 面板角落  A 自动弹反  X 自动惩戒  K 一击必杀  J 中/英  Q 退出\n",
           "  Ctrl+Alt+L bar  M position  P panel  O panel corner  A auto parry  X auto finisher  K one-hit kill  J language  Q quit\n"));

    OverlayInit(GetModuleHandleW(nullptr), WndProc);
    HWND hk = OverlayHotkeyWindow();
    AutoInit(hk);
    std::wstring taken;
    for (auto& k : kHotkeys)
        if (!RegisterHotKey(hk, k.id, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, k.vk)) taken += (wchar_t)k.vk;
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    TrayAdd(hk);
    if (taken.empty()) Toast(TR("SBParry 已启动 · 托盘图标可设置", "SBParry started · settings in tray icon"));
    else Toast(std::wstring(TR("快捷键被别的程序占用：Ctrl+Alt+", "Hotkeys taken by another program: Ctrl+Alt+")) + taken +
               TR("（可用托盘菜单）", " (use the tray menu)"));

    // 每个合成帧（显示器刷新）更新一次；DwmFlush 失败时退回 ~250Hz
    MSG m;
    for (bool run = true; run;) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { run = false; break; }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!run) break;
        if (FAILED(DwmFlush())) Sleep(4);
        if (WaitForSingleObject(quitEvent, 0) == WAIT_OBJECT_0) { Log(TR("[SBParry] 退出（--quit）\n", "[SBParry] Quit (--quit)\n")); break; }
        Tick();
    }
    AutoRelease();
    SetOneHitKill(false);
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
    OverlayShutdown();
    Detach(true);
    return 0;
}
