// 自动弹反（默认关，Ctrl+Alt+A）：按判定条预测的时刻替玩家按键
//   可弹反的攻击：完美窗口正中按格挡；只能完美闪避的：按闪避；蓝光/紫光：窗口内“前推/后拉 + 闪避”
// 按键用玩家自己在游戏里的键位（SBParryBridge 导出 keys.txt），输入设备跟随玩家最近使用的：
//   键盘 SendInput；Xbox/XInput 手柄和 DualSense 通过游戏内的导入表钩子叠加按键（见 hooks.asm）
#pragma once
#include "common.h"

void AutoTick();       // 每帧调用
void AutoRelease();    // 松开所有模拟按键（关闭/退出时）
void AutoInit(HWND w); // 注册 Raw Input，区分玩家真实的键鼠操作和本程序模拟的
void AutoOnRawInput(LPARAM lp); // WM_INPUT
const wchar_t* AutoDeviceName(); // 当前使用的输入设备（显示用）
