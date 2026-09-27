// 不依赖 UE4SS 的数据导出：后台线程直接读游戏的 UE 反射信息（ue.cpp），产出和 SBParryBridge 完全相同格式的
// steps.tsv / live.txt / projectiles.txt / keys.txt 文本。读取方统一走 BridgeText：有原生导出就用它，
// 没有（特征码没对上等）再读 Bridge 写的文件
#pragma once
#include "common.h"

enum class BridgeFile { Steps, Live, Projectiles, Keys, Count };

// Attach 找到对象数组 / 名字池后启动；Detach 时停止
void NativeStart(DWORD pid, uint64_t objArray, uint64_t namePool);
void NativeStop();
bool NativeActive(); // 原生导出已经产出步骤表

enum class TextState { Unchanged, Changed, Missing, Stale };
// 取某个文件的最新内容。stamp 记着上次读到的版本，内容没变返回 Unchanged（text 不动）；
// Bridge 文件比游戏进程还旧（上一局留下的）返回 Stale
TextState BridgeText(BridgeFile which, std::string& text, uint64_t& stamp);

// 按行读一段文本（fgets 的替代）
bool NextLine(const std::string& s, size_t& pos, char* buf, size_t n);
