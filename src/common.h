// SBParry — Stellar Blade parry / dodge timing trainer
// Shared includes and small helpers.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cstdarg>
#include <share.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

// 60 帧换算：游戏按真实 dt 扣剩余时间，这里只用于把秒显示成“几帧”
constexpr double kFps = 60.0;
constexpr double kDegToRad = 3.14159265358979 / 180;

extern double g_tscPerSec;
inline double TscToSec(int64_t d) { return (double)d / g_tscPerSec; }
inline uint64_t SecToTsc(double s) { return (uint64_t)(s * g_tscPerSec); }
inline int Frames(double sec) { return (int)std::lround(sec * kFps); }
inline int Ms(double sec) { return (int)std::lround(sec * 1000); }

#define SBP_VERSION L"0.2.1"

// 中英双语：TR("中文", "English")。语言在 config 里选，默认跟随游戏 / 系统
enum class Lang { Zh, En };
extern Lang g_lang;
extern int g_logEnglish; // > 0：正在拼日志文字，TR 一律取英文
#define TR(zh, en) (g_lang == Lang::Zh && !g_logEnglish ? L##zh : L##en)

// 这个作用域里的 TR 都取英文
struct EnglishScope {
    EnglishScope() { g_logEnglish++; }
    ~EnglishScope() { g_logEnglish--; }
    EnglishScope(const EnglishScope&) = delete;
};

// 写 sbparry.log（UTF-8，程序目录下；托盘菜单“打开日志文件”）。日志不论界面语言一律英文：
// Log(...) 的参数（包括里面的 TR、AutoDeviceName 等）都在 EnglishScope 里求值
void LogImpl(const wchar_t* fmt, ...);
#define Log(...)                     \
    do {                             \
        EnglishScope sbpLogEnglish_; \
        LogImpl(__VA_ARGS__);        \
    } while (0)
