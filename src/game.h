// 进程、钩子与游戏内存读取
#pragma once
#include "common.h"

// 游戏里的事件环形缓冲区条目（由 hooks.asm 写入）
#pragma pack(push, 1)
struct LogEntry {
    uint64_t tsc;
    uint64_t inst;     // 技能实例
    float remain;      // 判定/按下：inst+B8 完美窗口剩余；步骤：本步已过时间
    float total;       // 判定/按下：inst+BC 完美窗口总长；步骤：新步骤时长
    uint32_t kind;     // 1=判定 2=按下 3=步骤切换
    uint32_t seq;      // 写入序号：取槽时先写 ~序号，其余字段写完后才翻成序号，读的一方据此判断条目已写完
    uint64_t row;      // 判定/按下：技能表行；步骤：步骤表行
    uint64_t attacker; // 判定：攻击者；步骤：技能组件
};
#pragma pack(pop)
static_assert(sizeof(LogEntry) == 0x30, "LogEntry layout");

enum : uint32_t { kLogJudge = 1, kLogPress = 2, kLogStep = 3 };

// 注入用的共享控制块（在 cave 里，本程序写、游戏里的手柄钩子读）
#pragma pack(push, 1)
struct PadCtrl {
    // XInput（XInputGetState）
    uint32_t xiActive;    // 非 0 时把下面的按键/摇杆叠加到游戏读到的状态上
    uint16_t xiButtons;   // XINPUT_GAMEPAD_* 位
    int16_t xiLX, xiLY;   // 摇杆覆盖值（xiStick 非 0 时生效）
    uint16_t xiStick;
    uint64_t xiLastReal;  // 玩家真实操作手柄的最后 TSC（用来判断当前输入设备）
    uint64_t xiCalls;     // 调用计数（调试）
    // DualSense 原生（libScePad: scePadReadState）
    uint32_t psActive;
    uint32_t psButtons;   // SCE_PAD_BUTTON_* 位
    uint8_t psLX, psLY;   // 摇杆覆盖值（psStick 非 0 时生效），128 居中
    uint16_t psStick;
    uint64_t psLastReal;
    uint64_t psCalls;
};
#pragma pack(pop)

struct Game {
    HANDLE h = nullptr;
    DWORD pid = 0;
    uint64_t base = 0, imageSize = 0;
    uint64_t judge = 0, press = 0, step = 0; // 钩子点
    uint64_t cave = 0, log = 0, ctrl = 0;    // 远程分配的 cave、事件日志、手柄控制块
    uint64_t tramp[3] = {};
    uint64_t iatXInput = 0, iatScePad = 0;   // 手柄钩子改写的导入表槽位
    uint32_t readIdx = 0;
    FILETIME start{};                        // 游戏进程启动时间（判断 Bridge 导出的文件是不是这一局的）
    bool ok = false;
};
extern Game g;

bool Attach();
void Detach(bool restore);
void RetryUeGlobals(); // 连接时 UE 全局量还没初始化：定期重试，找到后切到原生导出
void RefreshPadHooks(); // 延迟加载的导入可能在之后才被解析、覆盖掉我们的槽位，定期检查补挂

bool Rpm(uint64_t a, void* out, size_t n);
bool Wpm(uint64_t a, const void* src, size_t n);      // 改代码：临时改页保护
bool WriteData(uint64_t a, const void* src, size_t n); // 写 cave 里的数据（本来就可写）
template <class T> bool Read(uint64_t a, T& v) { return Rpm(a, &v, sizeof(T)); }

std::wstring GameDir(); // ...\SB\Binaries\Win64
std::wstring BridgeDir(); // GameDir()\ue4ss\Mods\SBParryBridge
const FILETIME& GameStartTime();
