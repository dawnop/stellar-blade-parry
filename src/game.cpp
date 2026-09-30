// 进程附加、钩子安装/还原、手柄导入表钩子
//
// 钩子点都用特征码定位（游戏小更新后通常仍能对上）：
//   判定  IsJustActionActive 入口                       5 字节  -> jmp 跳板
//   按下  movss [r14+BC],xmm0（设置完美窗口）           9 字节  -> jmp 跳板 + nop
//   步骤  mov [r13+B4],ecx（切换技能步骤）              7 字节  -> jmp 跳板 + nop
// 远程分配的 cave 可能离模块超过 ±2GB，rel32 够不着：钩子点先跳到模块内 0xCC 对齐填充里的 14 字节跳板
// “jmp [rip+0]; dq cave块”，块尾同样用绝对跳转回去。
// 手柄：把导入表里 XInputGetState / scePadReadState 的槽位指向 cave 里的小函数（不改游戏代码）。
#include "game.h"
#include "config.h"
#include "native.h"
#include <set>
#include <tlhelp32.h>

Game g;

extern "C" const uint8_t sbp_judge_begin[], sbp_judge_end[], sbp_press_begin[], sbp_press_end[], sbp_step_begin[],
    sbp_step_end[], sbp_xinput_begin[], sbp_xinput_end[], sbp_scepad_begin[], sbp_scepad_end[];

static const wchar_t* kExeName = L"SB-Win64-Shipping.exe";

// 代码所在范围（相对模块基址）
static const uint64_t kScanBegin = 0x1000;
static const uint64_t kScanEnd = 0x55AD000;

static const char* kSigJudge = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC A0 00 00 00 80 B9 B4 01 00 00 00";
static const char* kSigJudgeHooked = "E9 ?? ?? ?? ?? 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC A0 00 00 00 80 B9 B4 01 00 00 00";
static const char* kSigPress = "F3 41 0F 11 86 BC 00 00 00 85 FF 74";
static const char* kSigStep = "F3 41 0F 11 85 B0 00 00 00 8B 48 18 41 89 8D B4 00 00 00 74";
static const uint64_t kSigStepHookOff = 0xC;
// UE 全局对象：GUObjectArray（mov [rip+x],eax 写 ObjFirstGCIndex）、FNamePool（lea rcx,[rip+x]; call 构造; mov byte [已初始化],1）
static const char* kSigObjArray = "89 05 ?? ?? ?? ?? 85 DB 7F 36 4C 8D 05";
static const char* kSigNamePool = "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01";
static const uint8_t kOrigJudge[5] = {0x48, 0x89, 0x5C, 0x24, 0x08};
static const uint8_t kOrigPress[9] = {0xF3, 0x41, 0x0F, 0x11, 0x86, 0xBC, 0x00, 0x00, 0x00};
static const uint8_t kOrigStep[7] = {0x41, 0x89, 0x8D, 0xB4, 0x00, 0x00, 0x00};

// hooks.asm 里的占位常量；每块最后 8 字节是回跳地址 / 原函数地址
static const uint64_t kMagicLog = 0x5342504C4F474731ull, kMagicCtrl = 0x5342504354524C31ull;

// cave 布局
static const size_t kCaveJudge = 0x000, kCaveLog = 0x100, kCavePress = 0x3200, kCaveStep = 0x3300, kCaveXInput = 0x3400,
                    kCaveScePad = 0x3500, kCaveCtrl = 0x3E00, kCaveHeader = 0x3F00, kCaveSize = 0x4000;
static const int kRingBytes = 0x10 + 256 * (int)sizeof(LogEntry);
static_assert(kCaveLog + kRingBytes <= kCavePress, "cave layout");
// 头部记录钩子位置，程序崩溃/强关没还原时，下次启动据此还原。SBP2 是旧版（前 6 个字段相同）
static const uint32_t kCaveMagic = 0x33504253, kCaveMagicOld = 0x32504253; // "SBP3" / "SBP2"
struct CaveHeader {
    uint32_t magic, pad;
    uint64_t judge, press, step, tramp[3];
    uint64_t iatXInput, origXInput, iatScePad, origScePad;
};

static FILE* g_logFile;

void LogImpl(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (!g_logFile) g_logFile = _wfsopen(L"sbparry.log", L"ab", _SH_DENYWR);
    if (g_logFile) {
        char u8[1536];
        int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8), nullptr, nullptr);
        if (n > 1) { fwrite(u8, 1, n - 1, g_logFile); fflush(g_logFile); }
    }
}

bool Rpm(uint64_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(g.h, (LPCVOID)a, out, n, &got) && got == n;
}

bool Wpm(uint64_t a, const void* src, size_t n) {
    DWORD old = 0;
    if (!VirtualProtectEx(g.h, (LPVOID)a, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    SIZE_T put = 0;
    BOOL r = WriteProcessMemory(g.h, (LPVOID)a, src, n, &put);
    VirtualProtectEx(g.h, (LPVOID)a, n, old, &old);
    FlushInstructionCache(g.h, (LPCVOID)a, n);
    return r && put == n;
}

bool WriteData(uint64_t a, const void* src, size_t n) {
    SIZE_T put = 0;
    return WriteProcessMemory(g.h, (LPVOID)a, src, n, &put) && put == n;
}

std::wstring GameDir() {
    wchar_t p[MAX_PATH];
    DWORD n = MAX_PATH;
    if (!g.h || !QueryFullProcessImageNameW(g.h, 0, p, &n)) return L"";
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L'\\'));
}

std::wstring BridgeDir() { return GameDir() + L"\\ue4ss\\Mods\\SBParryBridge"; }

const FILETIME& GameStartTime() { return g.start; }

// ---------------------------------------------------------------- 特征码

static std::vector<int> ParsePattern(const char* s) {
    std::vector<int> out;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') {
            out.push_back(-1);
            while (*s && *s != ' ') s++;
        } else {
            out.push_back((int)strtoul(s, nullptr, 16));
            s += 2;
        }
    }
    return out;
}

// 在模块代码段里扫多个特征码，返回每个的全部命中
static std::vector<std::vector<uint64_t>> ScanAll(const std::vector<std::vector<int>>& pats) {
    std::vector<std::vector<uint64_t>> hits(pats.size());
    const size_t chunk = 4 << 20, overlap = 64;
    std::vector<uint8_t> buf(chunk + overlap);
    for (uint64_t off = kScanBegin; off < kScanEnd; off += chunk) {
        size_t n = (size_t)std::min<uint64_t>(chunk + overlap, kScanEnd - off);
        SIZE_T got = 0;
        if (!ReadProcessMemory(g.h, (LPCVOID)(g.base + off), buf.data(), n, &got) || got < 32) continue;
        for (size_t p = 0; p < pats.size(); p++) {
            const auto& pat = pats[p];
            size_t lim = got >= pat.size() ? got - pat.size() : 0;
            size_t stop = std::min(lim + 1, chunk); // 重叠区只在下一块里算
            for (size_t i = 0; i < stop; i++) {
                if (buf[i] != pat[0]) continue;
                size_t k = 1;
                while (k < pat.size() && (pat[k] < 0 || buf[i + k] == pat[k])) k++;
                if (k == pat.size()) hits[p].push_back(g.base + off + i);
            }
        }
    }
    return hits;
}

// 在模块代码段里找 n 段 >=16 字节的 0xCC 对齐填充（前面至少还有 1 个 0xCC），用来放跳板
static std::vector<uint64_t> FindPadding(uint64_t around, int n) {
    std::vector<uint64_t> out;
    const size_t span = 32 << 20;
    uint64_t from = std::max<uint64_t>(g.base + kScanBegin, around > span ? around - span : 0);
    uint64_t to = std::min<uint64_t>(g.base + kScanEnd, around + span);
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t a = from; a < to && (int)out.size() < n; a += buf.size()) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(g.h, (LPCVOID)a, buf.data(), buf.size(), &got)) continue;
        for (size_t i = 16; i + 32 <= got && (int)out.size() < n; i += 16) {
            if (buf[i - 1] != 0xCC) continue; // 前面也是填充，保证不是指令里恰好的 CC
            bool ok = true;
            for (int k = 0; k < 16 && ok; k++) ok = buf[i + k] == 0xCC;
            if (ok) { out.push_back(a + i); i += 16; }
        }
    }
    return out;
}

static void PutAbsJmp(uint8_t* p, uint64_t to) {
    static const uint8_t op[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(p, op, 6);
    memcpy(p + 6, &to, 8);
}

static int32_t Rel32(uint64_t from_next, uint64_t to) { return (int32_t)((int64_t)to - (int64_t)from_next); }

// 把 hooks.asm 的一个块拷进 cave 缓冲区（槽位 at 起 room 字节）：替换占位常量，最后 8 字节填 tail
static bool PlaceBlock(std::vector<uint8_t>& cave, size_t at, size_t room, const uint8_t* b, const uint8_t* e, uint64_t tail) {
    size_t n = (size_t)(e - b);
    if (n < 16 || n > room) return false;
    memcpy(&cave[at], b, n);
    for (size_t i = at; i + 8 <= at + n - 8; i++) {
        uint64_t v;
        memcpy(&v, &cave[i], 8);
        if (v == kMagicLog) memcpy(&cave[i], &g.log, 8);
        if (v == kMagicCtrl) memcpy(&cave[i], &g.ctrl, 8);
    }
    memcpy(&cave[at + n - 8], &tail, 8);
    return true;
}

// ---------------------------------------------------------------- 导入表

// 在游戏内存里的 PE 导入表中找某个导入函数的 IAT 槽位。name 为空时按序号 ordinal 找。delay=延迟加载导入表
static uint64_t FindImportSlot(const char* dll, const char* name, uint16_t ordinal, bool delay) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!Read(g.base, dos) || !Read(g.base + dos.e_lfanew, nt)) return 0;
    auto& dir = nt.OptionalHeader.DataDirectory[delay ? IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT : IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    auto readStr = [](uint64_t a) {
        char s[128] = {};
        Rpm(a, s, sizeof(s) - 1);
        return std::string(s);
    };
    for (uint64_t d = g.base + dir.VirtualAddress;; d += delay ? sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) : sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        uint32_t nameRva, intRva, iatRva;
        if (delay) {
            IMAGE_DELAYLOAD_DESCRIPTOR dd;
            if (!Read(d, dd) || !dd.DllNameRVA) return 0;
            nameRva = dd.DllNameRVA, intRva = dd.ImportNameTableRVA, iatRva = dd.ImportAddressTableRVA;
        } else {
            IMAGE_IMPORT_DESCRIPTOR id;
            if (!Read(d, id) || !id.Name) return 0;
            nameRva = id.Name, intRva = id.OriginalFirstThunk, iatRva = id.FirstThunk;
        }
        if (_stricmp(readStr(g.base + nameRva).c_str(), dll)) continue;
        for (int i = 0;; i++) {
            uint64_t th = 0;
            if (!Read(g.base + intRva + i * 8ull, th) || !th) return 0;
            bool match = (th >> 63) ? (!name && (uint16_t)th == ordinal) : (name && readStr(g.base + (uint32_t)th + 2) == name);
            if (match) return g.base + iatRva + i * 8ull;
        }
    }
}

// 槽位没指向我们的函数就（重新）指过去：先记下原值（延迟加载在首次调用时才把槽位改成真实地址）
static void HookSlot(uint64_t slot, uint64_t stub, size_t stubSize, size_t headerField) {
    if (!slot) return;
    uint64_t cur = 0;
    if (!Read(slot, cur) || !cur || cur == stub) return;
    if (!Wpm(stub + stubSize - 8, &cur, 8)) return; // 块尾：原函数
    Wpm(g.cave + kCaveHeader + headerField, &cur, 8);
    Wpm(slot, &stub, 8);
}

void RefreshPadHooks() {
    if (!g.ok) return;
    HookSlot(g.iatXInput, g.cave + kCaveXInput, sbp_xinput_end - sbp_xinput_begin, offsetof(CaveHeader, origXInput));
    HookSlot(g.iatScePad, g.cave + kCaveScePad, sbp_scepad_end - sbp_scepad_begin, offsetof(CaveHeader, origScePad));
}

static void UnhookSlot(uint64_t slot, uint64_t stub, uint64_t orig) {
    uint64_t cur = 0;
    if (slot && orig && Read(slot, cur) && cur == stub) Wpm(slot, &orig, 8);
}

// ---------------------------------------------------------------- 附加 / 还原

static void RestoreFrom(const CaveHeader& h, uint64_t cave) {
    // 只还原原始指令，跳板和 cave 都留着：游戏线程可能刚跳进去、正在里面
    Wpm(h.judge, kOrigJudge, sizeof(kOrigJudge));
    Wpm(h.press, kOrigPress, sizeof(kOrigPress));
    if (h.step) Wpm(h.step, kOrigStep, sizeof(kOrigStep));
    if (h.magic == kCaveMagic) {
        // 松开模拟按键（崩溃时可能正按着），再把手柄导入表指回去
        PadCtrl zero{};
        WriteData(cave + kCaveCtrl, &zero, sizeof(zero));
        UnhookSlot(h.iatXInput, cave + kCaveXInput, h.origXInput);
        UnhookSlot(h.iatScePad, cave + kCaveScePad, h.origScePad);
    }
}

// 连接时扫到的对象数组 / 名字池特征码命中，UE 全局量还没初始化时留着重试
static std::vector<uint64_t> g_objHits, g_poolHits;
static bool g_ueRetry;
static ULONGLONG g_ueTriedAt;

void Detach(bool restore) {
    NativeStop();
    g_ueRetry = false;
    if (g.h && restore && g.cave) {
        CaveHeader h{};
        if (Read(g.cave + kCaveHeader, h)) RestoreFrom(h, g.cave);
        Log(TR("[SBParry] 已还原游戏代码\n", "[SBParry] Game code restored\n"));
    }
    if (g.h) CloseHandle(g.h);
    g = Game{};
}

static DWORD FindPid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, kExeName)) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

static uint64_t FindModuleBase(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{sizeof(me)};
    uint64_t base = 0;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        if (!_wcsicmp(me.szModule, kExeName)) { base = (uint64_t)me.modBaseAddr; g.imageSize = me.modBaseSize; break; }
    CloseHandle(snap);
    return base;
}

// 上次没还原（崩溃/强关）：钩子点 -> 跳板 -> cave，按头部记录还原原始代码
static bool RestoreStale(uint64_t hookedJudge) {
    int32_t rel = 0;
    Rpm(hookedJudge + 1, &rel, 4);
    uint64_t t = hookedJudge + 5 + rel, cave = 0;
    uint8_t op[6] = {};
    Rpm(t, op, 6);
    if (op[0] == 0xFF && op[1] == 0x25) Rpm(t + 6, &cave, 8); else cave = t;
    CaveHeader h{};
    if (!Rpm(cave + kCaveHeader, &h, sizeof(h)) || (h.magic != kCaveMagic && h.magic != kCaveMagicOld) || h.judge != hookedJudge)
        return false;
    RestoreFrom(h, cave);
    Log(TR("[SBParry] 已清理上次残留的钩子\n", "[SBParry] Cleaned up hooks left by a previous run\n"));
    return true;
}

// rip 相对寻址的目标（disp32 在 at+dispOff，指令长 len）
static uint64_t RipTarget(uint64_t at, int dispOff, int len) {
    int32_t d = 0;
    return Read(at + dispOff, d) ? at + len + d : 0;
}

// 对象数组和名字池：命中里挑出结构对得上的那个，找到就启动原生导出（不需要 UE4SS）。
// 游戏刚启动时两者还没初始化（对象数太少 / 名字池为空），先退回 Bridge，之后每隔几秒用同一批命中重试（RetryUeGlobals）
static bool FindUeGlobals(bool first) {
    if (g_cfg.dataSource == DataSource::Ue4ss) {
        Log(L"[SBParry] dataSource=ue4ss: reading data from SBParryBridge only\n");
        return false;
    }
    uint64_t objArray = 0, pool = 0;
    for (uint64_t a : g_objHits) {
        uint64_t x = RipTarget(a, 2, 6), chunks = 0, first = 0;
        int32_t num = 0;
        if (Read(x + 0x10, chunks) && Read(x + 0x24, num) && num > 1000 && num < 8 * 1024 * 1024 && Read(chunks, first) && first) {
            objArray = x;
            break;
        }
    }
    std::set<uint64_t> tried;
    for (uint64_t a : g_poolHits) {
        uint64_t x = RipTarget(a, 3, 7), blk = 0;
        if (!tried.insert(x).second) continue;
        uint8_t e[6] = {};
        if (Read(x + 0x10, blk) && blk && Rpm(blk, e, 6) && ((e[0] | e[1] << 8) >> 6) == 4 && !memcmp(e + 2, "None", 4)) {
            pool = x;
            break;
        }
    }
    if (objArray && pool) {
        if (!first) Log(L"[SBParry] UE globals found, reading data directly from the game\n");
        NativeStart(g.pid, objArray, pool);
        return true;
    }
    if (first)
        Log(L"[SBParry] UE globals not found yet (objects %s, names %s)%s; retrying\n", objArray ? L"OK" : L"-", pool ? L"OK" : L"-",
            g_cfg.dataSource == DataSource::Native ? L"" : L"; reading data from SBParryBridge (UE4SS) meanwhile");
    return false;
}

void RetryUeGlobals() {
    if (!g.ok || !g_ueRetry || GetTickCount64() - g_ueTriedAt < 3000) return;
    g_ueTriedAt = GetTickCount64();
    if (FindUeGlobals(false)) g_ueRetry = false;
}

// 连接失败：还没写钩子点前分配的 cave 可以直接释放（跳板没有任何代码跳进去）
static bool AttachFailed(const wchar_t* msg) {
    if (msg) Log(L"%s", msg);
    if (g.cave) VirtualFreeEx(g.h, (LPVOID)g.cave, 0, MEM_RELEASE);
    Detach(false);
    return false;
}

bool Attach() {
    // 同一个进程连不上（特征码对不上 / 权限不够）就放慢到 30 秒一试：每次都要扫 90MB 代码、写日志
    static DWORD failedPid = 0;
    static ULONGLONG failedAt = 0;
    DWORD pid = FindPid();
    if (!pid || (pid == failedPid && GetTickCount64() - failedAt < 30000)) return false;
    failedPid = pid, failedAt = GetTickCount64(); // 成功时清掉
    g.h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!g.h) { Log(TR("[SBParry] 打开游戏进程失败 %lu\n", "[SBParry] OpenProcess failed %lu\n"), GetLastError()); return false; }
    g.pid = pid;
    FILETIME x1, x2, x3;
    GetProcessTimes(g.h, &g.start, &x1, &x2, &x3);
    g.base = FindModuleBase(pid);
    if (!g.base) { failedPid = 0; return AttachFailed(nullptr); } // 刚启动、模块还没载入：稍后再试

    auto sigs = std::vector<std::vector<int>>{ParsePattern(kSigJudge), ParsePattern(kSigJudgeHooked), ParsePattern(kSigPress),
                                              ParsePattern(kSigStep), ParsePattern(kSigObjArray), ParsePattern(kSigNamePool)};
    auto hits = ScanAll(sigs);
    if (hits[0].empty() && hits[1].size() == 1) {
        if (!RestoreStale(hits[1][0])) {
            return AttachFailed(L"[SBParry] The judge function was modified by something else; giving up\n");
        }
        hits = ScanAll(sigs);
    }
    if (hits[0].size() != 1 || hits[2].size() != 1 || hits[3].size() != 1) {
        Log(TR("[SBParry] 特征码没对上（judge=%zu press=%zu step=%zu），游戏可能更新了\n",
               "[SBParry] Signatures not found (judge=%zu press=%zu step=%zu); the game may have been updated\n"),
            hits[0].size(), hits[2].size(), hits[3].size());
        return AttachFailed(nullptr);
    }
    g.judge = hits[0][0];
    g.press = hits[2][0];
    g.step = hits[3][0] + kSigStepHookOff;
    auto pads = FindPadding(g.judge, 3);
    if (pads.size() < 3) return AttachFailed(L"[SBParry] No code padding for trampolines\n");
    g.cave = (uint64_t)VirtualAllocEx(g.h, nullptr, kCaveSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g.cave) { Log(TR("[SBParry] 分配内存失败 %lu\n", "[SBParry] VirtualAllocEx failed %lu\n"), GetLastError()); return AttachFailed(nullptr); }
    g.log = g.cave + kCaveLog;
    g.ctrl = g.cave + kCaveCtrl;
    g.iatXInput = FindImportSlot("XINPUT1_3.dll", nullptr, 2, false); // 序号 2 = XInputGetState
    g.iatScePad = FindImportSlot("libScePad.dll", "scePadReadState", 0, true);

    std::vector<uint8_t> cave(kCaveSize, 0);
    if (!PlaceBlock(cave, kCaveJudge, kCaveLog - kCaveJudge, sbp_judge_begin, sbp_judge_end, g.judge + 5) ||
        !PlaceBlock(cave, kCavePress, kCaveStep - kCavePress, sbp_press_begin, sbp_press_end, g.press + 9) ||
        !PlaceBlock(cave, kCaveStep, kCaveXInput - kCaveStep, sbp_step_begin, sbp_step_end, g.step + 7) ||
        !PlaceBlock(cave, kCaveXInput, kCaveScePad - kCaveXInput, sbp_xinput_begin, sbp_xinput_end, 0) ||
        !PlaceBlock(cave, kCaveScePad, kCaveCtrl - kCaveScePad, sbp_scepad_begin, sbp_scepad_end, 0))
        return AttachFailed(L"[SBParry] hooks.asm block too large for its cave slot\n");
    // 空槽的 seq 设成不会等于读序号的值（全 0 的槽 0 会被当成已写完的序号 0）
    for (int i = 0; i < 256; i++) {
        uint32_t none = ~0u;
        memcpy(&cave[kCaveLog + 0x10 + i * sizeof(LogEntry) + offsetof(LogEntry, seq)], &none, 4);
    }
    CaveHeader hdr{kCaveMagic, 0, g.judge, g.press, g.step, {pads[0], pads[1], pads[2]}, g.iatXInput, 0, g.iatScePad, 0};
    memcpy(&cave[kCaveHeader], &hdr, sizeof(hdr));
    if (!Wpm(g.cave, cave.data(), cave.size())) return AttachFailed(L"[SBParry] Write failed\n");
    // 跳板
    const uint64_t blocks[3] = {g.cave + kCaveJudge, g.cave + kCavePress, g.cave + kCaveStep};
    for (int i = 0; i < 3; i++) {
        uint8_t t[14];
        PutAbsJmp(t, blocks[i]);
        if (!Wpm(pads[i], t, sizeof(t))) return AttachFailed(L"[SBParry] Trampoline write failed\n");
    }
    // 钩子点（最后写，写完即生效）
    uint8_t pj[5] = {0xE9}, pp[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90}, ps[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    int32_t rj = Rel32(g.judge + 5, pads[0]), rp = Rel32(g.press + 5, pads[1]), rs = Rel32(g.step + 5, pads[2]);
    memcpy(pj + 1, &rj, 4);
    memcpy(pp + 1, &rp, 4);
    memcpy(ps + 1, &rs, 4);
    g.ok = true; // Detach(true) 依赖 cave 头部，现在已写好
    if (!Wpm(g.step, ps, sizeof(ps)) || !Wpm(g.press, pp, sizeof(pp)) || !Wpm(g.judge, pj, sizeof(pj))) {
        Log(TR("[SBParry] 写钩子失败\n", "[SBParry] Hook write failed\n"));
        Detach(true);
        return false;
    }
    failedPid = 0;
    g_objHits = hits[4], g_poolHits = hits[5];
    g_ueRetry = !FindUeGlobals(true) && g_cfg.dataSource != DataSource::Ue4ss;
    g_ueTriedAt = GetTickCount64();
    RefreshPadHooks();
    Rpm(g.log, &g.readIdx, 4);
    Log(TR("[SBParry] 已连接游戏 pid=%lu（判定 +%llX 按下 +%llX 步骤 +%llX，手柄 XInput %s / DualSense %s）\n",
           "[SBParry] Attached pid=%lu (judge +%llX press +%llX step +%llX, pad XInput %s / DualSense %s)\n"),
        pid, g.judge - g.base, g.press - g.base, g.step - g.base, g.iatXInput ? L"OK" : L"-", g.iatScePad ? L"OK" : L"-");
    return true;
}
