// 叠加层绘制：判定条、统计面板、屏幕提示（GDI+ 抗锯齿 + 逐像素 alpha 的分层窗口）
#pragma once
#include "common.h"
#include "tracker.h"

// 每帧交给叠加层的全部状态（叠加层只负责画，不读游戏）
struct HudFrame {
    bool connected;                      // 已挂上游戏
    const wchar_t* status;               // 非空：显示一行状态（等待游戏 / Bridge 未安装 …）
    double window;                       // 当前完美窗口（秒，已按整帧折算）
    double horizon;                      // 判定条显示未来多少秒
    const std::vector<Note>* notes;      // 按 t 升序
    const std::deque<Result>* results;   // 新的在前
    bool autoOn;                         // 自动弹反开着（判定条上显示 AUTO 标记）
    const wchar_t* toast;                // 快捷键切换提示，空=不显示
    double toastAge;                     // 提示已显示秒数
    float targetDistance;                // 到锁定敌人的距离（米），<0 未知
    double now;                          // 秒（单调），用于动画
};

void OverlayInit(HINSTANCE inst, WNDPROC hotkeyProc); // 创建窗口；hotkeyProc 接收 WM_HOTKEY 等消息
HWND OverlayHotkeyWindow();                           // 注册快捷键用的窗口

// 判定条窗口的尺寸（按 g_cfg.uiScale 缩放后），放置位置由调用方算
SIZE OverlayBarSize();
// visible=false 时隐藏；x,y 为窗口左上角屏幕坐标
void OverlayDrawBar(const HudFrame& f, int x, int y, bool visible);
// 统计面板：屏幕 rect 内的角落（corner 0..3）；visible=false 隐藏
void OverlayDrawPanel(const HudFrame& f, const RECT& screen, int corner, bool visible);
// 屏幕中上方的提示（快捷键切换、首次启动说明），screen 为游戏客户区
void OverlayDrawToast(const HudFrame& f, const RECT& screen);
void OverlayShutdown();
