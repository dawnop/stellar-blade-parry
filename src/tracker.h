// 步骤表、音符预测与判定结算
#pragma once
#include "common.h"

// 判定条上的一个音符
enum class NoteKind : uint8_t {
    Parry,   // 可完美弹反（也可完美闪避）
    Evade,   // 只能完美闪避（抓取等）
    Danger,  // 不能弹反也不能完美闪避，只能躲开
    Jump,    // 贴地扩散的冲击波环（渡鸦后跳连段落地等）：闪避躲不掉，要跳起来
    Blink,   // 蓝光：窗口内“前推 + 闪避”闪到敌人身后
    Repulse, // 紫光：窗口内“后拉 + 闪避”
};

struct Note {
    NoteKind kind;
    double t;        // 距现在多少秒：命中结算时刻 / 蓝紫窗口开始
    double len;      // 蓝紫窗口长度（秒），其余为 0
    uint64_t inst;   // 敌方技能实例（归不到步骤的飞行道具：道具地址）
    int step;        // 步骤表序号（-1：归不到步骤的飞行道具）
    uint64_t atTsc;  // t 对应的绝对 TSC（去重用）
    float range;     // 蓝紫窗口的触发距离（米），其余为 0
    float rate;      // 敌人当前的游戏时间流速（弹反后的顿帧里 < 1），t 是按游戏时间算的
    double tShow;    // 判定条上显示用的 t：远程攻击的预测会随距离、道具出现而修正，显示时不让点跳，见 SmoothShown
    uint64_t proj;   // 实时追踪的飞行道具地址（其余为 0）：一轮连发的几发归到同一步骤上，靠它区分
};

// 一次判定结果
enum class Action : uint8_t { Parry, Evade, Blink, Repulse };
enum class Verdict : uint8_t {
    Perfect, // 完美弹反/闪避；蓝紫：成功触发
    Early,   // 早了（off = 早了多少秒）
    Late,    // 晚了（off = 晚了多少秒）
    Missed,  // 蓝紫：按在窗口内但没触发（方向或距离不对）
    Unhandled, // 敌人攻击到了、伊芙没有任何格挡/闪避（多半挨打了）
};

struct Result {
    Action action;
    Verdict v;
    double lead;    // 弹反/闪避：按下到结算的秒数
    double window;  // 弹反/闪避：游戏窗口；蓝紫：窗口长度
    double off;     // 早/晚了多少秒（正数）
    ULONGLONG shownAt;
    bool automatic; // 由自动弹反触发
};

extern std::vector<Note> g_notes;     // 每帧重建，按 t 升序
extern std::deque<Result> g_results;  // 新的在前
extern double g_window;               // 最近一次完美窗口（秒，按整帧折算后的有效长度）

bool StepsLoaded();
bool LoadSteps();    // 读 Bridge 导出的 steps.tsv，与游戏内存核对
void ResetTracker(); // 游戏断开时清空
bool Poll();         // 读事件环形缓冲区，结算判定；读失败返回 false
// 每帧：根据敌方技能实例当前步骤和在飞的飞行道具推算音符（targetDist 用于按距离估算飞行时间）
void UpdateNotes(float targetDist);
// 每帧：伊芙血条（0~1，未知 <0）下降时记一条“受到伤害”，附最近的敌方攻击，用来查漏掉的招
void TrackHp(float pct);

// 自动操作用
void MarkAutomatic(Action a, uint64_t tsc); // 自动按下时登记，结果上标记“自动”
double InputLatency();                      // 自动按键到游戏收到的延迟（秒，实测中位数）
uint64_t LastEveStepTsc();                  // 伊芙最近一次切换步骤（确认自动按键被游戏接受）
const std::string& LastEveStepName();       // 伊芙最近进入的步骤名
uint64_t LastJustEvadeTsc();                // 伊芙最近一次完美闪避开始（之后一小段无敌）
bool MashActive();                          // 敌人处在“挣脱”步骤（拼刀 / 被抓）：需要连打轻攻击

// 一击必杀（跳过阶段用）：伊芙所有攻击步骤的伤害倍率 ×1000；关闭 / 退出时还原
void SetOneHitKill(bool on);
bool OneHitKillOn();

void OpenTimingLog(); // debugLog=1 时写 timing.csv
void TimingNote(const char* what, const Note& n, float eveScale); // 调试：记一条自动操作
