// sbparry.ini 设置（程序目录下，改了会自动保存）
#pragma once
#include "common.h"

enum class InputDevice { Auto, Keyboard, XInput, DualSense };
// 游戏数据从哪来：Auto=直接读游戏内存，读不到再用 SBParryBridge（UE4SS）；Native=只直接读；Ue4ss=只用 Bridge
enum class DataSource { Auto, Native, Ue4ss };

struct Config {
    int language = 0;       // 0=跟随系统 1=中文 2=English
    int barMode = 0;        // 判定条位置：0=血条下方 1=跟随敌人（限制在画面中央）
    bool barVisible = true; // 判定条
    bool panelVisible = false; // 统计面板
    int panelCorner = 0;    // 统计面板角落 0=左上 1=右上 2=左下 3=右下
    bool autoParry = false; // 自动弹反/闪避（默认关）
    bool autoChance = true; // 自动模式下也处理蓝紫光（前闪/后闪）
    bool autoQte = true;    // 自动模式下也按惩戒（敌人倒地时的 Y）和过场 QTE；拼刀、被抓时的连打挣脱不受影响
    InputDevice autoDevice = InputDevice::Auto; // 自动操作用哪种输入：Auto=跟随玩家最近使用的设备
    DataSource dataSource = DataSource::Auto;   // 启动时读取，改了要重启 SBParry
    int autoAimMs = 0;      // 自动按键时机微调（毫秒，正数=更晚）
    bool debugLog = false;  // 写 timing.csv（开发调试用）
    int uiScale = 100;      // 判定条缩放百分比
};

extern Config g_cfg;
void LoadConfig();
void SaveConfig();
