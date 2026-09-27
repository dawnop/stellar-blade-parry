// 步骤表、音符预测、判定结算
//
// 技能实例（全局池，伊芙和敌人共用）：+58 技能表行  +60 当前步骤行  +B0 本步已过时间  +B4 本步时长
//   +B8 完美窗口剩余  +BC 完美窗口总长
// 敌人一次攻击 = Cast(前摇) -> Hit(判定框)；沿 NextStepAlias 往后推算每个 Hit 到来的时刻。
//
// 完美判定（实测对照伊芙的 JustParry/Guard 步骤验证过）：
//   攻击判定框接触伊芙期间，IsJustActionActive 每帧调用一次（2~6 帧），游戏以**最后一次**调用结算：
//   那一帧窗口剩余 > 0 即完美。所以提前量 = 窗口总长 - 最后一帧剩余；有效窗口是整帧数（0.23s -> 13 帧）。
//   连按时旧技能实例也会收到接触，但游戏按最新一次按键结算。
//
// 蓝光/紫光是另一套机制：敌人 Cast 步骤开始时给自己挂 Chance_BehindSkill / Chance_MoveBackSkill 效果
// （startDelayTime 后生效，持续 Time 秒），效果在身时伊芙“前推/后拉 + 闪避”会变成 FlashBehindAttack / MoveBackAttack。
//
// 远程攻击：飞行道具（剑气等）的命中时刻由 projectiles.cpp 按实时位置和速度算；道具出现之前按“距离 / 速度”估。
// 贴地扩散的冲击波环（Hit 步骤在自身位置生成、范围随时间放大的效果）当作从初始半径出发、匀速扩散的“道具”，要跳过去。
#include "tracker.h"
#include "game.h"
#include "config.h"
#include "native.h"
#include "projectiles.h"

std::vector<Note> g_notes;
std::deque<Result> g_results;
double g_window = 0.217;

// ---------------------------------------------------------------- 步骤表

enum class EveMove : uint8_t { None, Evade, Blink, Repulse };

struct StepInfo {
    int type; // 0=Cast 1=Hit
    float dur;
    int next;
    bool jp, ja;  // 可完美弹反 / 可完美闪避
    float delay;  // 首个攻击判定框延迟，-1=无
    bool player;
    std::string name;
    NoteKind chance = NoteKind::Parry; // Blink / Repulse 有效
    float chStart = 0, chLen = 0, chRange = 0;
    EveMove eve = EveMove::None; // 伊芙的步骤：闪避 / 闪现 / 后撤的第一步
    bool justEvade = false;      // 伊芙完美闪避的第一步
    float reach = 0;             // 攻击范围（米），0=未知
    int real = 1;                // Hit 步骤是否真的打人（0=脚本演出：拼刀/抓取成功后的连段等，没有攻击碰撞也没有飞行道具）
    float speed = 0;             // 飞行道具速度（米/秒），0=近战。剑气等：结算 = 发射 + (距离 - ahead)/速度
    float ahead = -1;            // 飞行道具出发时离敌人多远（米）；扩散的冲击波环 = 初始半径。-1=未知，按 kProjAhead
    bool mash = false;           // 有 NextStepAliasWhenLinkBreak：拼刀/被抓，连打轻攻击挣脱（渡鸦、锯鲨、巨兽、Scarlet 等）
    bool HasChance() const { return chLen > 0; }
};
static std::vector<StepInfo> g_steps;
static std::map<uint64_t, int> g_stepByRow;
static std::vector<uint64_t> g_stepRow; // 序号 -> 游戏内存里的步骤表行

struct InstState {
    float lastB0 = -1;
    ULONGLONG lastChange = 0;
    int seenStep = -1;        // 上一帧读到的步骤（步骤刚切换时内存可能只更新了一半，等下一帧再用）
    uint64_t rateTsc = 0;     // 测游戏时间流速：上次采样的 TSC / 步骤 / 已过时间
    int rateStep = -1;
    float rateB0 = 0, rate = 1;
    int step = -1;            // 步骤事件记录的当前步骤
    uint64_t stepStart = 0;   // 该步骤开始的 TSC（步骤事件时间，精确）
    int lastHit = -1;         // 最近一个 Hit 步骤及其开始时刻（用于结算校准）
    uint64_t hitStart = 0;
    std::vector<Note> notes;  // 上一帧给这个实例出的音符：步骤切换那一帧不重算，沿用它（否则音符消失一帧，绿区会闪）
    uint64_t notesTsc = 0;
};
static std::map<uint64_t, InstState> g_insts;

bool StepsLoaded() { return !g_steps.empty(); }

// 在 before 之前最近开始的敌方 Hit 步骤（可加条件）。结算校准、飞行道具归属、掉血归因都要找“刚才是哪一招”
static int LatestEnemyHit(uint64_t before, uint64_t* start = nullptr, uint64_t* inst = nullptr,
                          bool (*pred)(const StepInfo&) = nullptr) {
    int step = -1;
    uint64_t best = 0;
    for (auto& [i, s] : g_insts)
        if (s.lastHit >= 0 && s.hitStart > best && s.hitStart < before && (!pred || pred(g_steps[s.lastHit]))) {
            best = s.hitStart, step = s.lastHit;
            if (inst) *inst = i;
        }
    if (start) *start = best;
    return step;
}
// 除 inst 以外，还有别的敌人在 [from, before) 之间开始过 Hit 步骤（多个敌人同时出招时，结算归不准是谁的）
static bool OtherEnemyHit(uint64_t inst, uint64_t from, uint64_t before) {
    for (auto& [i, s] : g_insts)
        if (i != inst && s.lastHit >= 0 && s.hitStart >= from && s.hitStart < before) return true;
    return false;
}
static bool IsProjectile(const StepInfo& s) { return s.speed > 0 && s.ahead < 0; }
static bool IsRing(const StepInfo& s) { return s.speed > 0 && s.ahead >= 0; }

// 敌方 Hit 步骤对应的音符类型
static NoteKind HitKind(const StepInfo& s) {
    if (IsRing(s)) return NoteKind::Jump; // 冲击波环：实测闪避（哪怕时机对）也会挨打，只能跳
    return s.jp ? NoteKind::Parry : s.ja ? NoteKind::Evade : NoteKind::Danger;
}

static EveMove ClassifyEve(const std::string& n) {
    if (n.rfind("P_Eve", 0) != 0 || n.find("_Cast1") == std::string::npos) return EveMove::None;
    if (n.find("FlashBehindAttack") != std::string::npos) return EveMove::Blink;
    if (n.find("MoveBackAttack") != std::string::npos) return EveMove::Repulse;
    if (n.find("Evade") != std::string::npos) return EveMove::Evade;
    return EveMove::None;
}

// ---------------------------------------------------------------- 结算校准
// 每个 Hit 步骤实测的“步骤开始 -> 结算”秒数（最近若干次）。同一招很稳定（各招 35~145ms 不等），按招校准。
// calib_default.tsv 随程序发布（Raven 全招式），calib.tsv 是本机学到的，优先

static const double kHitLag = 0.08; // 没校准过的招：按首个判定框延迟 + 80ms 估计
static std::map<int, std::deque<float>> g_calib;
static const size_t kCalibKeep = 9;

static void SaveCalib();
static float g_dist = -1; // 伊芙到敌人的距离（米），每帧更新
static uint64_t g_lastPerfectTsc; // 最近一次完美弹反/闪避：之后短时间内敌人的时间流速下降才是顿帧

// 飞行时间：按当前距离算，近战为 0。飞行道具出现后改用 projectiles.cpp 的实时追踪，这里只管出现之前的估计。
// 道具不是从敌人身上出发的：实测渡鸦剑气在它前方约 4.2m 处生成，离伊芙约 1.05m 时结算，出发距离未知的扣掉 5.2m
static const double kProjAhead = 5.2;
static double Flight(const StepInfo& s) {
    if (s.speed <= 0 || g_dist <= 0) return 0;
    return std::max(0.0, g_dist - (s.ahead >= 0 ? s.ahead : kProjAhead)) / s.speed;
}

// Hit 步骤开始 -> 结算。飞行道具校准的是“扣掉飞行时间后”的部分
static double HitOffset(int step) {
    const StepInfo& s = g_steps[step];
    auto it = g_calib.find(step);
    double base;
    if (it == g_calib.end() || it->second.empty()) {
        // 飞行道具：发射后按距离飞；范围判定（没有碰撞组，delay=-1，如 BurstAreaSlash_Hit1）步骤一开始就结算
        // 冲击波环：实测渡鸦的环 14m 处 0.63s、17.7m 处 0.835s 掉血（血条还有一两帧延迟），比“初始半径 + 扩散速度”早约 30ms
        base = IsRing(s) ? -0.03 : s.speed > 0 ? 0.03 : s.delay < 0 ? 0.01 : s.delay + kHitLag;
    } else {
        std::vector<float> v(it->second.begin(), it->second.end());
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        base = v[v.size() / 2];
    }
    return base + Flight(s);
}

static void AddCalib(int step, float settle) {
    auto& q = g_calib[step];
    q.push_back(settle - (float)Flight(g_steps[step]));
    if (q.size() > kCalibKeep) q.pop_front();
    SaveCalib();
}

static void SaveCalib() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"calib.tsv", L"wb") || !f) return;
    for (auto& [step, q] : g_calib) {
        fprintf(f, "%s", g_steps[step].name.c_str());
        for (float x : q) fprintf(f, "\t%.4f", x);
        fputc('\n', f);
    }
    fclose(f);
}

static int LoadCalibFile(const wchar_t* path, const std::map<std::string, int>& byName) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") || !f) return 0;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char* ctx = nullptr;
        char* tok = strtok_s(line, "\t\r\n", &ctx);
        auto it = tok ? byName.find(tok) : byName.end();
        if (it == byName.end()) continue;
        auto& q = g_calib[it->second];
        q.clear();
        while ((tok = strtok_s(nullptr, "\t\r\n", &ctx))) q.push_back((float)atof(tok));
        n++;
    }
    fclose(f);
    return n;
}

static void LoadCalib() {
    g_calib.clear();
    std::map<std::string, int> byName;
    for (int i = 0; i < (int)g_steps.size(); i++) byName[g_steps[i].name] = i;
    int d = LoadCalibFile(L"calib_default.tsv", byName);
    int u = LoadCalibFile(L"calib.tsv", byName);
    Log(TR("[SBParry] 结算校准：内置 %d 招，本机 %d 招\n", "[SBParry] Settle calibration: %d built-in, %d learned\n"), d, u);
}

// ---------------------------------------------------------------- 调试日志 timing.csv

static FILE* g_timing;
static uint64_t g_predHitTsc = 0; // 判定条预测的最近一次命中时刻，和实际结算对比

void OpenTimingLog() {
    if (g_timing || !g_cfg.debugLog) return;
    g_timing = _wfsopen(L"timing.csv", L"ab", _SH_DENYWR);
    if (g_timing && _ftelli64(g_timing) == 0)
        fputs("time,verdict,kind,window_ms,lead_ms,pred_err_ms,hit_step,type,settle_ms,delay_ms\n", g_timing);
}

static void TimingLine(const char* fmt, ...) {
    if (!g_timing) return;
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    fprintf(g_timing, "%02d:%02d:%02d.%03d,", lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_timing, fmt, ap);
    va_end(ap);
    fflush(g_timing);
}

void TimingNote(const char* what, const Note& n, float eveScale) {
    TimingLine("auto,%s,%s,%.0f,%.2f,%.2f,%.1f\n", what, n.step >= 0 ? g_steps[n.step].name.c_str() : "projectile", n.t * 1000, n.rate,
               eveScale, g_dist);
}

// ---------------------------------------------------------------- 载入

static void ResetJudge();

void ResetTracker() {
    SetOneHitKill(false);
    ResetJudge();
    ResetProjectiles();
    g_steps.clear();
    g_stepByRow.clear();
    g_insts.clear();
    g_notes.clear();
}

bool LoadSteps() {
    std::string text;
    uint64_t stamp = 0;
    if (BridgeText(BridgeFile::Steps, text, stamp) != TextState::Changed) return false;
    char line[512];
    uint64_t table = 0;
    std::vector<StepInfo> steps;
    for (size_t pos = 0; NextLine(text, pos, line, sizeof(line));) {
        if (line[0] == '#') { sscanf_s(line, "#table=0x%llx", &table); continue; }
        StepInfo si{};
        char name[256];
        int idx = 0, jp = 0, ja = 0, ck = 0, mash = 0;
        int n = sscanf_s(line, "%d\t%255s\t%d\t%f\t%d\t%d\t%d\t%f\t%d\t%f\t%f\t%f\t%d\t%f\t%f\t%d\t%f", &idx, name, (unsigned)sizeof(name),
                         &si.type, &si.dur, &si.next, &jp, &ja, &si.delay, &ck, &si.chStart, &si.chLen, &si.chRange, &mash, &si.speed, &si.reach, &si.real,
                         &si.ahead);
        if (n < 8) continue; // 旧版 bridge 少后面几列
        si.mash = mash != 0;
        si.jp = jp != 0;
        si.ja = ja != 0;
        if (ck == 1) si.chance = NoteKind::Blink;
        else if (ck == 2) si.chance = NoteKind::Repulse;
        else si.chLen = 0;
        si.player = name[0] == 'P' && name[1] == '_';
        si.name = name;
        si.eve = ClassifyEve(si.name);
        si.justEvade = si.player && si.name.find("JustEvade") != std::string::npos && si.name.find("_Cast1") != std::string::npos;
        if (idx < 0 || idx > 200000) continue;
        if (idx != (int)steps.size()) steps.resize(idx);
        steps.push_back(si);
    }
    uint64_t data = 0;
    int32_t num = 0;
    if (!table || steps.empty() || !Read(table + 0x30, data) || !Read(table + 0x38, num) || num != (int)steps.size())
        return false; // 表地址属于上一次启动的游戏，等 bridge 重新导出
    std::vector<uint8_t> buf((size_t)num * 0x18);
    if (!Rpm(data, buf.data(), buf.size())) return false;
    std::map<uint64_t, int> byRow;
    for (int i = 0; i < num; i++) {
        uint64_t row;
        memcpy(&row, &buf[(size_t)i * 0x18 + 8], 8);
        byRow[row] = i;
    }
    // 抽查几行时长，确认顺序对得上
    for (int i = 0; i < num; i += num / 7 + 1) {
        uint64_t row;
        memcpy(&row, &buf[(size_t)i * 0x18 + 8], 8);
        float d = 0;
        if (!Read(row + 0x18, d) || std::fabs(d - steps[i].dur) > 1e-3f) return false;
    }
    g_stepRow.assign(num, 0);
    for (auto& [row, i] : byRow) g_stepRow[i] = row;
    g_steps.swap(steps);
    g_stepByRow.swap(byRow);
    LoadCalib();
    Log(TR("[SBParry] 已载入步骤表 %d 行（%s）\n", "[SBParry] Step table loaded: %d rows (%s)\n"), num,
        NativeActive() ? L"read directly from the game" : L"from SBParryBridge");
    return true;
}

// ---------------------------------------------------------------- 结果

static uint64_t g_autoTsc[4]; // 各动作最近一次自动按下
static uint64_t g_autoPending; // 最近一次自动按格挡/闪避、还没对上游戏按下事件的时刻
static std::deque<float> g_latency;

void MarkAutomatic(Action a, uint64_t tsc) {
    g_autoTsc[(int)a] = tsc;
    if (a == Action::Parry || a == Action::Evade) g_autoPending = tsc;
}

// 模拟按键要等游戏下一次处理输入、再走完技能逻辑才生效，实测 3~5 帧；自动操作按这个提前量出手
double InputLatency() {
    if (g_latency.empty()) return 0.07;
    std::vector<float> v(g_latency.begin(), g_latency.end());
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

static bool WasAutomatic(Action a, uint64_t pressTsc) {
    uint64_t t = g_autoTsc[(int)a];
    return t && pressTsc >= t && pressTsc - t < SecToTsc(0.2);
}

static const wchar_t* ActionName(Action a) {
    switch (a) {
    case Action::Parry: return TR("弹反", "Parry");
    case Action::Evade: return TR("闪避", "Dodge");
    case Action::Blink: return TR("前闪", "Blink");
    default: return TR("后闪", "Repulse");
    }
}

static void Push(const Result& r) {
    g_results.push_front(r);
    if (g_results.size() > 40) g_results.pop_back();
    EnglishScope en; // 日志一律英文
    const wchar_t* tag = r.automatic ? L" [AUTO]" : L"";
    const wchar_t* a = ActionName(r.action);
    switch (r.v) {
    case Verdict::Perfect:
        if (r.action == Action::Parry || r.action == Action::Evade)
            Log(TR("  完美%s  提前 %4dms (%2df)  窗口 %dms%s\n", "  Perfect %s  lead %4dms (%2df)  window %dms%s\n"), a, Ms(r.lead),
                Frames(r.lead), Ms(r.window), tag);
        else
            Log(TR("  %s成功%s\n", "  %s!%s\n"), a, tag);
        break;
    case Verdict::Early: Log(TR("  %s  早了 %4dms (%2df)%s\n", "  %s  early %4dms (%2df)%s\n"), a, Ms(r.off), Frames(r.off), tag); break;
    case Verdict::Late: Log(TR("  %s  晚了 %4dms (%2df)%s\n", "  %s  late %4dms (%2df)%s\n"), a, Ms(r.off), Frames(r.off), tag); break;
    case Verdict::Missed: Log(TR("  %s  窗口内但没触发（方向/距离）%s\n", "  %s  in window but not triggered (direction/range)%s\n"), a, tag); break;
    case Verdict::Unhandled: Log(TR("  未应对：%s\n", "  Unhandled: %s\n"), a); break;
    }
}

// 技能表行里没有现成的类型字段：故事模式窗口 闪避 0.3 / 弹反 0.2
static std::map<uint64_t, bool> g_rowIsEvade;
static bool RowIsEvade(uint64_t row) {
    auto it = g_rowIsEvade.find(row);
    if (it != g_rowIsEvade.end()) return it->second;
    float story = 0;
    bool evade = Read(row + 0x10C, story) && story > 0.25f;
    g_rowIsEvade[row] = evade;
    return evade;
}

// ---------------------------------------------------------------- 弹反/闪避结算

struct Burst {
    uint64_t lastTsc = 0, pressTsc = 0, row = 0;
    float remain = 0, total = 0;
    bool done = false;
    int enemyStep = -1; // 结算那一帧敌人所在 Hit 步骤
    float enemyB0 = 0;  // 结算时该步骤已进行的秒数
    bool calib = false; // 只有这一个敌人在出招：可以拿来校准
};
static std::map<uint64_t, Burst> g_bursts; // 按攻击者归并
static std::map<uint64_t, uint64_t> g_pressTsc; // 伊芙技能实例 -> 最近一次按下

static void FinishBurst(Burst& bu) {
    bu.done = true;
    if (bu.calib && bu.enemyStep >= 0 && bu.enemyB0 > 0 && bu.enemyB0 < 0.4f + Flight(g_steps[bu.enemyStep])) AddCalib(bu.enemyStep, bu.enemyB0);
    Result r{};
    r.action = RowIsEvade(bu.row) ? Action::Evade : Action::Parry;
    r.window = bu.total;
    r.lead = (double)bu.total - (double)bu.remain;
    r.v = bu.remain > 0.f ? Verdict::Perfect : Verdict::Early;
    if (r.v == Verdict::Perfect) g_lastPerfectTsc = bu.lastTsc;
    r.off = r.v == Verdict::Early ? r.lead - r.window : 0;
    r.shownAt = GetTickCount64();
    r.automatic = WasAutomatic(r.action, bu.pressTsc);
    // 游戏每帧扣一次剩余时间、最后一帧剩余 > 0 才算完美：有效窗口是整帧数
    g_window = (std::ceil(bu.total * kFps) - 1) / kFps;
    Push(r);
    if (g_timing) {
        double predErr = g_predHitTsc ? TscToSec((int64_t)(bu.lastTsc - g_predHitTsc)) : 1e9;
        const StepInfo* es = bu.enemyStep >= 0 ? &g_steps[bu.enemyStep] : nullptr;
        TimingLine("%s,%s,%d,%d,%s,%s,%d,%.0f,%.0f\n", r.v == Verdict::Perfect ? "perfect" : "early",
                   r.action == Action::Evade ? "evade" : "parry", Ms(r.window), Ms(r.lead),
                   std::fabs(predErr) < 0.5 ? std::to_string(Ms(predErr)).c_str() : "", es ? es->name.c_str() : "",
                   es ? es->type : -1, es ? bu.enemyB0 * 1000 : 0, es ? std::max(0.f, es->delay) * 1000 : 0);
    }
}

// ---------------------------------------------------------------- 蓝紫光结算

struct ChanceWin {
    uint64_t inst;
    int step;
    uint64_t start, end; // TSC
    NoteKind kind;
    bool judged;
};
static std::deque<ChanceWin> g_chances;

struct PendingHit { uint64_t start, deadline; int step; };
static std::deque<PendingHit> g_pendingHits;
static uint64_t g_eveActTsc; // 伊芙最近一次格挡/闪避类步骤
static uint64_t g_eveStepTsc; // 伊芙最近一次任何步骤切换
uint64_t LastEveStepTsc() { return g_eveStepTsc; }
static std::string g_eveStepName;
const std::string& LastEveStepName() { return g_eveStepName; }
static uint64_t g_justEvadeTsc; // 伊芙最近一次完美闪避（JustEvade 第一步）开始
uint64_t LastJustEvadeTsc() { return g_justEvadeTsc; }

static void ResetJudge() {
    g_chances.clear();
    g_pendingHits.clear();
    g_bursts.clear();
    g_pressTsc.clear();
    g_rowIsEvade.clear();
}

// 敌人进入带机会效果的步骤：按步骤开始时刻 + startDelayTime 记下窗口
static void AddChance(uint64_t inst, int step, uint64_t stepStart) {
    const StepInfo& s = g_steps[step];
    uint64_t a = stepStart + SecToTsc(s.chStart), b = a + SecToTsc(s.chLen);
    for (auto& w : g_chances)
        if (w.inst == inst && w.step == step && (w.start > a ? w.start - a : a - w.start) < SecToTsc(0.1)) return;
    g_chances.push_back({inst, step, a, b, s.chance, false});
    while (g_chances.size() > 16) g_chances.pop_front();
}

// 伊芙开始闪避/闪现/后撤：对照最近的机会窗口给结果
static void JudgeChance(EveMove m, uint64_t tsc) {
    const uint64_t slack = SecToTsc(0.4);
    ChanceWin* best = nullptr;
    for (auto& w : g_chances)
        if (!w.judged && tsc + slack >= w.start && tsc <= w.end + slack) best = &w;
    if (!best) return;
    Result r{};
    r.action = best->kind == NoteKind::Blink ? Action::Blink : Action::Repulse;
    r.window = TscToSec((int64_t)(best->end - best->start));
    r.shownAt = GetTickCount64();
    if (m == EveMove::Blink || m == EveMove::Repulse) {
        r.action = m == EveMove::Blink ? Action::Blink : Action::Repulse;
        r.v = Verdict::Perfect;
    } else if (tsc < best->start) {
        r.v = Verdict::Early;
        r.off = TscToSec((int64_t)(best->start - tsc));
    } else if (tsc > best->end) {
        r.v = Verdict::Late;
        r.off = TscToSec((int64_t)(tsc - best->end));
    } else {
        r.v = Verdict::Missed;
    }
    // 步骤切换比按键晚 1~2 帧，自动按键的记录往前放宽
    uint64_t at = g_autoTsc[(int)r.action];
    r.automatic = at && tsc >= at && tsc - at < SecToTsc(0.25);
    best->judged = true;
    Push(r);
    TimingLine("chance,%s,%s,%d,%d\n", r.action == Action::Blink ? "blink" : "repulse",
               r.v == Verdict::Perfect ? "ok" : r.v == Verdict::Early ? "early" : r.v == Verdict::Late ? "late" : "missed",
               Ms(r.window), Ms(r.off));
}

// ---------------------------------------------------------------- 飞行道具的完美闪避
// 飞行道具的完美判定不走 IsJustActionActive，判定钩子看不到；但完美闪避成功时伊芙会切到 JustEvade 步骤，据此给结果。
// 近战的完美闪避在这之前已有判定钩子的接触记录（g_bursts），跳过。
static void JudgeProjectileEvade(uint64_t tsc) {
    const uint64_t span = SecToTsc(0.3);
    for (auto& [k, bu] : g_bursts)
        if (bu.lastTsc + span > tsc && bu.lastTsc < tsc + span) return;
    uint64_t press = 0;
    for (auto& [inst, t] : g_pressTsc) press = std::max(press, t);
    Result r{};
    r.action = Action::Evade;
    r.v = Verdict::Perfect;
    r.window = 0.23;
    g_lastPerfectTsc = tsc;
    r.lead = press && tsc > press && tsc - press < SecToTsc(0.4) ? TscToSec((int64_t)(tsc - press)) : 0;
    r.shownAt = GetTickCount64();
    r.automatic = WasAutomatic(Action::Evade, press);
    Push(r);
    // 结算校准：最近开始的飞行道具 / 冲击波 Hit 步骤
    uint64_t best = 0;
    int step = LatestEnemyHit(tsc, &best, nullptr, [](const StepInfo& s) { return s.speed > 0; });
    if (step >= 0 && tsc - best >= SecToTsc(1.5)) step = -1; // 很久以前的远程攻击，这次多半是范围攻击
    if (step >= 0) AddCalib(step, (float)TscToSec((int64_t)(tsc - best)));
    TimingLine("perfect,evade,projectile,%d,%s\n", Ms(r.lead), step >= 0 ? g_steps[step].name.c_str() : "");
}

// ---------------------------------------------------------------- 未应对的攻击
// 敌人 Hit 步骤结算前后伊芙没有任何格挡/闪避/闪现/跳跃，且敌人在近身范围（飞行道具不限距离），就记一条“未应对”。
// 不一定挨打了（可能本来就够不着）；真正掉血看下面的 TrackHp。

static bool IsEveResponse(const std::string& n) {
    for (const char* k : {"Guard", "Evade", "JustParry", "FlashBehind", "MoveBack", "Parry", "Jump"})
        if (n.find(k) != std::string::npos) return true;
    return false;
}

static void CheckPendingHits(uint64_t now) {
    while (!g_pendingHits.empty() && now >= g_pendingHits.front().deadline) {
        PendingHit h = g_pendingHits.front();
        g_pendingHits.pop_front();
        const StepInfo& s = g_steps[h.step];
        bool handled = g_eveActTsc + SecToTsc(0.7) > h.start && g_eveActTsc < h.deadline;
        for (auto& [k, bu] : g_bursts)
            if (bu.lastTsc > h.start && bu.lastTsc < h.deadline) handled = true;
        // 够不着的不算：近战按攻击范围（表里没有就按 3.5m），飞行道具不限距离
        float reach = s.reach > 0 ? s.reach + 1.f : 3.5f;
        if (handled || (s.speed == 0 && (g_dist < 0 || g_dist > reach))) continue;
        Result r{};
        r.action = s.jp ? Action::Parry : Action::Evade;
        r.v = Verdict::Unhandled;
        r.shownAt = GetTickCount64();
        Push(r);
        TimingLine("unhandled,%s,%.1f\n", s.name.c_str(), g_dist);
    }
}

// ---------------------------------------------------------------- 受到伤害（血条）
// 血条掉血有动画（约 150ms 分好几帧降下去），连续下降算同一次：第一帧的时刻≈挨打时刻，停止下降 0.3 秒后记一条总量
void TrackHp(float pct) {
    static float last = -1, before = -1;
    static uint64_t lastDropTsc;
    static std::string name;
    static int ms;
    uint64_t now = __rdtsc();
    auto flush = [&] {
        if (before < 0) return;
        Log(TR("  受到伤害 -%.1f%%（%hs，出招后 %dms）\n", "  Took damage -%.1f%% (%hs, %dms after it started)\n"), (before - last) * 100,
            name.c_str(), ms);
        before = -1;
    };
    if (pct < 0) { flush(); last = -1; return; }
    if (last >= 0 && pct < last - 0.0005f) {
        if (before < 0) {
            // 最近开始的敌方 Hit 步骤（飞行道具/冲击波可能在步骤结束后才打到）
            uint64_t best = 0;
            int step = LatestEnemyHit(now, &best);
            bool recent = step >= 0 && now - best < SecToTsc(3);
            name = recent ? g_steps[step].name : "?";
            ms = recent ? Ms(TscToSec((int64_t)(now - best))) : 0;
            before = last;
            TimingLine("damage,%s,%d,%.1f\n", name.c_str(), ms, g_dist);
        }
        lastDropTsc = now;
    } else if (before >= 0 && now - lastDropTsc > SecToTsc(0.3)) {
        flush();
    }
    last = pct;
}

// ---------------------------------------------------------------- 事件轮询

bool Poll() {
    uint32_t idx = 0;
    if (!Rpm(g.log, &idx, 4)) return false;
    const uint32_t ring = 256;
    if (idx - g.readIdx > ring) g.readIdx = idx - ring; // 太久没读，丢掉最老的
    const uint64_t gap = SecToTsc(3.5 / kFps);          // 超过 3 帧没接触 = 新的一次攻击
    for (; g.readIdx != idx; g.readIdx++) {
        // 先看 seq：槽已经取了、但游戏线程还没写完的条目留到下一帧再读
        uint64_t at = g.log + 0x10 + (uint64_t)(g.readIdx % ring) * sizeof(LogEntry);
        uint32_t seq = 0;
        if (!Read(at + offsetof(LogEntry, seq), seq)) return false;
        if (seq != g.readIdx) break;
        LogEntry e;
        if (!Rpm(at, &e, sizeof(e))) return false;
        if (!e.inst) continue;
        InstState& st = g_insts[e.inst];
        if (e.kind == kLogStep) {
            auto sit = g_stepByRow.find(e.row);
            if (sit == g_stepByRow.end()) continue;
            const StepInfo& s = g_steps[sit->second];
            st.step = sit->second;
            st.stepStart = e.tsc;
            if (s.HasChance() && !s.player) AddChance(e.inst, sit->second, e.tsc);
            if (s.eve != EveMove::None) JudgeChance(s.eve, e.tsc);
            if (s.justEvade) g_justEvadeTsc = e.tsc, JudgeProjectileEvade(e.tsc);
            if (s.player) g_eveStepTsc = e.tsc, g_eveStepName = s.name;
            if (s.player && IsEveResponse(s.name)) g_eveActTsc = e.tsc;
            if (!s.player && s.type == 1 && s.real && !s.mash)
                g_pendingHits.push_back({e.tsc, e.tsc + SecToTsc(HitOffset(sit->second) + 0.35), sit->second});
            if (g_timing)
                TimingLine("step,%s,,,,,%llu\n", s.name.c_str(), (unsigned long long)e.tsc);
            continue;
        }
        if (e.kind == kLogPress) {
            g_pressTsc[e.inst] = e.tsc;
            if (g_autoPending && e.tsc > g_autoPending && e.tsc - g_autoPending < SecToTsc(0.25)) {
                g_latency.push_back((float)TscToSec((int64_t)(e.tsc - g_autoPending)));
                if (g_latency.size() > 15) g_latency.pop_front();
                g_autoPending = 0;
            }
            continue;
        }
        if (e.kind != kLogJudge || e.total <= 0.f || !e.row) continue;
        uint64_t press = g_pressTsc.count(e.inst) ? g_pressTsc[e.inst] : 0;
        // 按攻击者归并：连按格挡时旧技能实例也会收到接触，但游戏按最新一次按键结算，旧实例的记录丢掉
        auto it = g_bursts.find(e.attacker);
        if (it != g_bursts.end() && !it->second.done && e.tsc - it->second.lastTsc > gap) FinishBurst(it->second);
        Burst& bu = g_bursts[e.attacker];
        if (bu.done || e.tsc - bu.lastTsc > gap) bu = Burst{};
        else if (press < bu.pressTsc) continue;
        // 接触持续好几帧，游戏在最后一帧结算：先记着，过了 gap 没有新接触再结算
        bu.lastTsc = e.tsc;
        bu.pressTsc = press;
        bu.remain = e.remain;
        bu.total = e.total;
        bu.row = e.row;
        // 最近开始的敌方 Hit 步骤：结算时刻 - 步骤开始 = 这一招的结算延迟
        uint64_t best = 0, inst = 0;
        bu.enemyStep = LatestEnemyHit(e.tsc, &best, &inst);
        bu.enemyB0 = bu.enemyStep >= 0 ? (float)TscToSec((int64_t)(e.tsc - best)) : 0;
        bu.calib = bu.enemyStep >= 0 && !OtherEnemyHit(inst, e.tsc - SecToTsc(1.5), e.tsc);
    }
    uint64_t now = __rdtsc();
    for (auto& [k, bu] : g_bursts)
        if (!bu.done && now - bu.lastTsc > gap) FinishBurst(bu);
    CheckPendingHits(now);
    return true;
}

// ---------------------------------------------------------------- 一击必杀
// 步骤表行 +0x20 SkillAttackDamageRate / +0x24 SkillShieldAttackDamageRate（伊芙的攻击步骤通常都是 1.0）。
// 直接改游戏内存里的表（堆内存，本来就可写），原值记下来，关闭或退出时写回。

struct SavedRate { uint64_t row; float v[2]; };
static std::vector<SavedRate> g_killSaved;
static const float kKillMul = 1000.f;

bool OneHitKillOn() { return !g_killSaved.empty(); }

void SetOneHitKill(bool on) {
    if (!on) {
        for (const SavedRate& s : g_killSaved)
            if (g.ok) WriteData(s.row + 0x20, s.v, sizeof(s.v));
        g_killSaved.clear();
        return;
    }
    if (OneHitKillOn() || !g.ok || g_steps.empty()) return;
    for (size_t i = 0; i < g_steps.size(); i++) {
        if (!g_steps[i].player || !g_stepRow[i]) continue;
        SavedRate s{g_stepRow[i]};
        if (!Rpm(s.row + 0x20, s.v, sizeof(s.v)) || (s.v[0] <= 0 && s.v[1] <= 0)) continue;
        float boosted[2] = {s.v[0] * kKillMul, s.v[1] * kKillMul};
        if (WriteData(s.row + 0x20, boosted, sizeof(boosted))) g_killSaved.push_back(s);
    }
    Log(TR("[SBParry] 一击必杀：已修改 %d 个攻击步骤\n", "[SBParry] One-hit kill: %d attack steps boosted\n"), (int)g_killSaved.size());
}

// ---------------------------------------------------------------- 音符

static bool g_mash;
bool MashActive() { return g_mash; }


// 远程攻击（飞行道具、冲击波环）的判定条显示：道具出现前按“距离 / 速度”估，出现后换成实时追踪，
// 估计值一修正，点就会跳。显示的到达时刻每秒最多修正 kShowFix 秒（点只是变快或变慢，不会跳），很快收敛到真实值；
// 自动操作仍用未平滑的 t。近战不平滑：它们本来就稳，弹反后的顿帧停顿也要照实显示
static const double kShowFix = 1.0;
struct ShownHit { double at; uint64_t seenTsc; }; // at：显示用的到达时刻（秒，TSC 换算）
static std::map<std::pair<uint64_t, int>, ShownHit> g_shown;

static void SmoothShown(uint64_t now) {
    const double nowSec = TscToSec((int64_t)now);
    for (Note& n : g_notes) {
        n.tShow = n.t;
        bool ranged = n.len == 0 && (n.step < 0 || g_steps[n.step].speed > 0);
        if (!ranged) continue;
        ShownHit& h = g_shown[{n.proj ? n.proj : n.inst, n.step}];
        double at = nowSec + n.t;
        double dt = h.seenTsc ? TscToSec((int64_t)(now - h.seenTsc)) : 1e9;
        // 断了（上一帧没有这个音符）或差得太远（已经是下一发了）就直接跳到新值
        if (dt > 0.2 || std::fabs(at - h.at) > 0.6) h.at = at;
        else h.at += std::clamp(at - h.at, -kShowFix * dt, kShowFix * dt);
        h.seenTsc = now;
        n.tShow = h.at - nowSec;
    }
    for (auto it = g_shown.begin(); it != g_shown.end();)
        it = now - it->second.seenTsc > SecToTsc(1) ? g_shown.erase(it) : std::next(it);
}

void UpdateNotes(float targetDist) {
    g_dist = targetDist;
    g_notes.clear();
    g_mash = false;
    if (g_steps.empty()) return;
    ULONGLONG nowMs = GetTickCount64();
    uint64_t now = __rdtsc();
    // 实时追踪的飞行道具：算到的是真实的命中时刻。归到最近发射飞行道具的敌方 Hit 步骤上（和按步骤推算的音符去重）；
    // 有道具在飞时，那一步按“距离/速度”推算的音符就不要了
    static std::vector<ProjHit> ph;
    const bool flying = ProjectilesTick(ph);
    uint64_t ownerInst = 0, ownerStart = 0;
    int ownerStep = LatestEnemyHit(now, &ownerStart, &ownerInst, IsProjectile);
    if (ownerStep >= 0 && now - ownerStart > SecToTsc(3)) ownerStep = -1;
    auto tracked = [&](uint64_t inst, int step) { return flying && inst == ownerInst && step == ownerStep; };
    for (const ProjHit& h : ph) {
        // 调试：每帧的实时预测（查判定条上点的漂移）
        TimingLine("proj,%llx,%.0f,%.1f,%.2f,%.2f\n", (unsigned long long)h.addr, h.t * 1000, h.speed, h.dmin, h.dist);
        NoteKind k = h.jp ? NoteKind::Parry : h.ja ? NoteKind::Evade : NoteKind::Danger;
        g_notes.push_back({k, h.t, 0, ownerStep >= 0 ? ownerInst : h.addr, ownerStep, now + SecToTsc(h.t), 0, 1.f, 0, h.addr});
    }
    for (auto& [inst, st] : g_insts) {
        uint8_t b[0x60];
        if (!Rpm(inst + 0x58, b, sizeof(b))) continue;
        uint64_t row;
        float b0, b4;
        memcpy(&row, b + 0x08, 8);
        memcpy(&b0, b + 0x58, 4);
        memcpy(&b4, b + 0x5C, 4);
        if (b0 != st.lastB0) { st.lastB0 = b0; st.lastChange = nowMs; }
        if (nowMs - st.lastChange > 300) continue; // 技能已结束，实例闲置
        auto it = g_stepByRow.find(row);
        if (it == g_stepByRow.end()) continue;
        const int cur = it->second;
        const StepInfo& cs = g_steps[cur];
        if (cs.player) continue;
        // 游戏线程正在切步骤时，我们可能读到“新步骤 + 旧的已过时间/时长”，推算会偏早半秒以上；
        // 步骤变化的这一帧先不出音符，下一帧读到的就一致了
        if (cur != st.seenStep || b0 > b4 + 0.001f) {
            st.seenStep = cur;
            if (now - st.notesTsc < SecToTsc(0.1)) {
                double dt = TscToSec((int64_t)(now - st.notesTsc));
                for (Note n : st.notes) { n.t -= dt; g_notes.push_back(n); }
            }
            continue;
        }
        const size_t first = g_notes.size();
        // 游戏时间流速：弹反成功后有顿帧，敌人的步骤时间走得比真实时间慢（甚至停住）。
        // 音符的 t 是游戏时间，顿帧期间真实到达时间要按流速换算，否则自动按键会偏早
        if (st.rateStep == cur && b0 < st.rateB0) {
            st.rateTsc = now, st.rateB0 = b0; // 步骤从头再来（循环到自己）：重新取样
        } else if (st.rateStep == cur && st.rateTsc && now - st.rateTsc > SecToTsc(0.03)) {
            float r = (float)((b0 - st.rateB0) / TscToSec((int64_t)(now - st.rateTsc)));
            st.rate = std::clamp(r, 0.f, 1.5f);
            st.rateTsc = now, st.rateB0 = b0;
        } else if (st.rateStep != cur || !st.rateTsc) {
            st.rateStep = cur, st.rateTsc = now, st.rateB0 = b0, st.rate = 1;
        }
        if (cs.mash) g_mash = true;
        // 当前步骤开始时刻：有步骤事件就用事件时间（精确），否则按已过时间倒推
        uint64_t start = now - SecToTsc(b0);
        if (st.step == cur && st.stepStart && st.stepStart <= now && now - st.stepStart < SecToTsc(b0 + 0.1)) start = st.stepStart;
        if (cs.type == 1 && (st.lastHit != cur || (start > st.hitStart ? start - st.hitStart : st.hitStart - start) > SecToTsc(0.05))) {
            st.lastHit = cur;
            st.hitStart = start;
        }
        double elapsed = TscToSec((int64_t)(now - start));
        // 流速只在完美弹反/闪避后的 0.5 秒内算顿帧；其他时候步骤时间停住多半是在等条件（比如后跳要等落地），不能当顿帧
        float rate = g_lastPerfectTsc && now - g_lastPerfectTsc < SecToTsc(0.5) ? st.rate : 1.f;
        auto push = [&](NoteKind k, double t, double len, int step, float range) {
            g_notes.push_back({k, t, len, inst, step, now + (uint64_t)((int64_t)(t * g_tscPerSec)), range, rate});
        };
        if (cs.type == 1 && cs.real && !tracked(inst, cur)) {
            double off = HitOffset(cur);
            if (elapsed < off + 0.07) push(HitKind(cs), off - elapsed, 0, cur, 0);
        }
        // 飞行道具：发射步骤很短（渡鸦后跳剑气的 Hit 步骤只有 0.1s），离得远时步骤已经切到下一个、剑气还在飞，
        // 音符要一直留到剑气打到
        if (st.lastHit >= 0 && st.lastHit != cur && st.hitStart && st.hitStart < now) {
            const StepInfo& h = g_steps[st.lastHit];
            double t = HitOffset(st.lastHit) - TscToSec((int64_t)(now - st.hitStart));
            if (h.speed > 0 && h.real && t > -0.07 && t < 3 && !tracked(inst, st.lastHit)) push(HitKind(h), t, 0, st.lastHit, 0);
        }
        // 当前步骤的蓝紫窗口按游戏时间算（完美闪避/闪现后的慢动作里游戏时间比真实时间慢）；
        // 用步骤事件时间当窗口标识，自动操作靠它去重，不能用会随慢动作漂移的估算值
        if (cs.HasChance() && cs.chStart + cs.chLen > b0) {
            uint64_t id = (st.step == cur && st.stepStart ? st.stepStart : start) + SecToTsc(cs.chStart);
            g_notes.push_back({cs.chance, cs.chStart - b0, cs.chLen, inst, cur, id, cs.chRange, rate});
        }
        double t = (double)b4 - b0;
        int s = cs.next;
        for (int k = 0; k < 10 && s >= 0 && s < (int)g_steps.size() && t < 1.5; k++) {
            const StepInfo& n = g_steps[s];
            if (n.type == 1 && n.real) push(HitKind(n), t + HitOffset(s), 0, s, 0);
            if (n.HasChance()) push(n.chance, t + n.chStart, n.chLen, s, n.chRange);
            t += n.dur;
            s = n.next;
        }
        st.notes.assign(g_notes.begin() + first, g_notes.end());
        st.notesTsc = now;
    }
    // 窗口比步骤长时（如 BetaGrab_Cast1 1.4s，窗口到 1.533s），步骤结束后的余下部分从步骤事件记录里画
    for (const ChanceWin& w : g_chances)
        if (w.end > now && !(g_insts.count(w.inst) && g_insts[w.inst].step == w.step)) {
            const StepInfo& s = g_steps[w.step];
            double t = TscToSec((int64_t)(w.start - now));
            g_notes.push_back({w.kind, t, TscToSec((int64_t)(w.end - w.start)), w.inst, w.step, w.start, s.chRange, 1.f});
        }
    SmoothShown(now);
    std::sort(g_notes.begin(), g_notes.end(), [](const Note& a, const Note& b) { return a.tShow < b.tShow; });
    for (const Note& n : g_notes)
        if (n.len == 0 && n.t >= 0 && n.t < 0.1) { g_predHitTsc = n.atTsc; break; }
}
