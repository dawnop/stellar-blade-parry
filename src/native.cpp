#include "native.h"
#include "config.h"
#include "game.h"
#include "ue.h"
#include <mutex>
#include <regex>
#include <set>

// 输出格式与 mod/SBParryBridge/Scripts/main.lua 逐字节一致（各段注释见那边），改一边就要改另一边

static const char* const kFileNames[] = {"steps.tsv", "live.txt", "projectiles.txt", "keys.txt"};

static std::mutex g_mu;
static std::string g_text[(int)BridgeFile::Count];
static uint64_t g_gen[(int)BridgeFile::Count];
static HANDLE g_thread;
static volatile LONG g_stop;
static DWORD g_pid;
static uint64_t g_objArray, g_namePool;
static ULONGLONG g_startedAt; // 原生导出启动时刻：刚启动的几秒里先等它，不急着读 Bridge 的文件
static const ULONGLONG kWaitNativeMs = 10000;

static std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static void Publish(BridgeFile f, const std::string& s) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_text[(int)f] == s) return;
        g_text[(int)f] = s;
        g_gen[(int)f]++;
    }
    // 调试：写一份到程序目录，和 Bridge 的文件对比
    if (g_cfg.debugLog) {
        std::string path = std::string("native_") + kFileNames[(int)f];
        FILE* fp = nullptr;
        if (!fopen_s(&fp, path.c_str(), "wb") && fp) { fwrite(s.data(), 1, s.size(), fp); fclose(fp); }
    }
}

// ---------------------------------------------------------------- steps.tsv

struct ChanceInfo { int kind; double life, range; };
struct ProjInfo { bool jp, ja; double speed; };
struct WaveInfo { bool zone; double speed, ahead; bool ja; };

static const std::regex kReObj(R"(\{[^{}]*\})"), kReAlias(R"re("Alias"\s*:\s*"([A-Za-z0-9_]+)")re"),
    kReTime(R"re("Time"\s*:\s*([0-9.]+))re"), kReDelay(R"re("startDelayTime"\s*:\s*([0-9.]+))re"),
    kReRange(R"(_(\d+)_\d+_\d+$)"), kReReach(R"(^ActionAssist_3D[A-Za-z]+_(\d+))"),
    kReDelayTime(R"re("DelayTime"\s*:\s*(-?[0-9.]+))re"), kReGroup(R"re("CollisionGroupName"\s*:\s*"([A-Za-z0-9_]+)")re");

// Lua tonumber：整段是数字才算
static bool ToNum(const std::string& s, double& v) {
    char* end = nullptr;
    v = strtod(s.c_str(), &end);
    return !s.empty() && end && !*end;
}

static void FindChance(const std::string& json, const std::map<std::string, ChanceInfo>& info, int& ck, double& cs, double& cl,
                       double& cr) {
    ck = 0, cs = cl = cr = 0;
    for (std::sregex_iterator it(json.begin(), json.end(), kReObj), end; it != end; ++it) {
        std::string obj = it->str();
        std::smatch m;
        if (!std::regex_search(obj, m, kReAlias)) continue;
        auto ci = info.find(m[1].str());
        if (ci == info.end()) continue;
        double t = ci->second.life, d = 0, v;
        if (std::regex_search(obj, m, kReTime) && ToNum(m[1].str(), v)) t = v;
        if (std::regex_search(obj, m, kReDelay) && ToNum(m[1].str(), v)) d = v;
        ck = ci->second.kind, cs = d, cl = t, cr = ci->second.range;
        return;
    }
}

static bool ExportSteps(std::string& out) {
    using namespace ue;
    Table st, et, pt, ft;
    if (!ReadTable("/Game/Local/Data/SkillActiveStepTable.SkillActiveStepTable", st) ||
        !ReadTable("/Game/Local/Data/EffectTable.EffectTable", et) || !ReadTable("/Game/Local/Data/ProjectileTable.ProjectileTable", pt) ||
        !ReadTable("/Game/Local/Data/TargetFilterTable.TargetFilterTable", ft))
        return false;
    Row row;
    // 效果表：蓝紫光机会、冲击波环、伤害区域
    Prop eActive = FindProp(et.rowStruct, "ActiveTargetFilterAlias"), eLife = FindProp(et.rowStruct, "LifeTime"),
         eLoop = FindProp(et.rowStruct, "LoopTargetFilterAlias"), eJa = FindProp(et.rowStruct, "AvailableJustEvade");
    if (!eActive || !eLife || !eLoop || !eJa) return false;
    // 目标过滤表：会扩散的范围
    struct Filter { double dist, lo, hi; }; // FarDistance / MinShapeScale / MaxShapeScale
    std::map<std::string, Filter> filters;
    Prop fDyn = FindProp(ft.rowStruct, "bDynamicShapeScale"), fMin = FindProp(ft.rowStruct, "MinShapeScale"),
         fMax = FindProp(ft.rowStruct, "MaxShapeScale"), fFar = FindProp(ft.rowStruct, "FarDistance");
    if (!fDyn || !fMin || !fMax || !fFar) return false;
    for (auto& [name, addr] : ft.rows) {
        if (!row.Load(addr, ft.rowSize)) continue;
        if (row.Bool(fDyn) && row.Float(fMax) > row.Float(fMin)) filters[name] = {row.Float(fFar), row.Float(fMin), row.Float(fMax)};
    }
    std::map<std::string, ChanceInfo> chances;
    std::map<std::string, WaveInfo> waves;
    for (auto& [name, addr] : et.rows) {
        if (!row.Load(addr, et.rowSize)) continue;
        int kind = !name.rfind("Chance_BehindSkill", 0) ? 1 : !name.rfind("Chance_MoveBackSkill", 0) ? 2 : 0;
        std::string active = row.FName(eActive), loop = row.FName(eLoop);
        double life = row.Float(eLife);
        if (kind) {
            std::smatch m;
            double range = std::regex_search(active, m, kReRange) ? strtod(m[1].str().c_str(), nullptr) / 100 : 0;
            chances[name] = {kind, life, range};
        }
        auto fl = filters.find(loop);
        if (fl != filters.end() && life > 0) {
            waves[name] = {false, fl->second.dist * (fl->second.hi - fl->second.lo) / life / 100, fl->second.dist * fl->second.lo / 100, row.Bool(eJa)};
        } else if (loop != "None" || (active != "None" && active != "Self")) {
            waves[name] = {true, 0, 0, row.Bool(eJa)};
        }
    }
    // 道具表
    std::map<std::string, ProjInfo> projs;
    Prop pJp = FindProp(pt.rowStruct, "AvailableJustParry"), pJa = FindProp(pt.rowStruct, "AvailableJustAction"),
         pSpeed = FindProp(pt.rowStruct, "Speed"), pMin = FindProp(pt.rowStruct, "MinSpeed"), pMax = FindProp(pt.rowStruct, "MaxSpeed"),
         pAcc = FindProp(pt.rowStruct, "Accelation");
    if (!pJp || !pJa || !pSpeed || !pMin || !pMax || !pAcc) return false;
    for (auto& [name, addr] : pt.rows) {
        if (!row.Load(addr, pt.rowSize)) continue;
        double v = row.Float(pSpeed), lo = row.Float(pMin), hi = row.Float(pMax);
        if (lo > 0 && v < lo) v = lo;
        if (hi > 0 && v > hi) v = hi;
        if (row.Float(pAcc) > 0 && hi > v) v = (v + hi) / 2;
        projs[name] = {row.Bool(pJp), row.Bool(pJa), v};
    }
    // 步骤表
    const uint64_t rs = st.rowStruct;
    Prop sType = FindProp(rs, "Type"), sDur = FindProp(rs, "Duration"), sNext = FindProp(rs, "NextStepAlias"),
         sJp = FindProp(rs, "AvailableJustParry"), sJa = FindProp(rs, "AvailableJustAction"),
         sStart = FindProp(rs, "StartSelfEffect"), sPaN = FindProp(rs, "UsableNonTargetProjectileAliasArray"),
         sPaT = FindProp(rs, "UsableTargetProjectileAliasArray"), sSelf = FindProp(rs, "CreateEffectSelfPosition"),
         sCga = FindProp(rs, "AttackCollisionGroupArray"), sOverride = FindProp(rs, "OverrideTargetFilterAlias"),
         sAssist = FindProp(rs, "ActionAssistTargetFilter"), sBreak = FindProp(rs, "NextStepAliasWhenLinkBreak");
    if (!sType || !sDur || !sNext || !sJp || !sJa || !sStart || !sPaN || !sPaT || !sSelf || !sCga || !sOverride || !sAssist || !sBreak)
        return false;

    struct R_ {
        std::string name, next;
        int type, ck, idx;
        double dur, delay, cs, cl, cr, speed, reach, ahead;
        bool jp, ja, mash, real;
    };
    std::vector<R_> rows;
    std::map<std::string, int> index;
    rows.reserve(st.rows.size());
    for (auto& [name, addr] : st.rows) {
        R_ r{};
        r.name = name;
        if (!row.Load(addr, st.rowSize)) return false;
        // 首个判定框的延迟：从 AttackCollisionGroupArray 的 JSON 字符串取（同 Bridge）；没有碰撞组 = -1
        std::string cga = row.Str(sCga);
        r.delay = -1;
        std::smatch m;
        if (std::regex_search(cga, m, kReObj)) {
            std::string first = m.str();
            double v;
            r.delay = std::regex_search(first, m, kReDelayTime) && ToNum(m[1].str(), v) ? v : 0;
        }
        FindChance(row.Str(sStart), chances, r.ck, r.cs, r.cl, r.cr);
        r.jp = row.Bool(sJp), r.ja = row.Bool(sJa), r.speed = 0;
        Prop pa = row.ArrNum(sPaN) > 0 ? sPaN : sPaT;
        int paNum = row.ArrNum(pa);
        if (paNum > 0) {
            uint32_t fn[2] = {};
            Read(row.ArrData(pa), fn, 8);
            auto p = projs.find(Name(fn[0], fn[1]));
            if (p != projs.end()) r.jp = p->second.jp, r.ja = p->second.ja, r.speed = p->second.speed / 100;
        }
        r.ahead = -1;
        bool isWave = false;
        std::string self = row.Str(sSelf);
        for (std::sregex_iterator it(self.begin(), self.end(), kReAlias), end; it != end; ++it) {
            auto w = waves.find((*it)[1].str());
            if (w == waves.end()) continue;
            if (!w->second.zone) r.speed = w->second.speed, r.ahead = w->second.ahead;
            r.ja = r.ja || w->second.ja, isWave = true;
            break;
        }
        r.type = row.Int(sType);
        r.dur = row.Float(sDur);
        r.next = row.FName(sNext);
        // 起手冲刺（只有 Collision_Dash 的 Cast 步骤，如红莲 SwingCombo_Cast1）：冲到身前就停，打不到人，配套的范围判定也不算（同 Bridge）
        bool dashOnly = name.find("_Cast") != std::string::npos && !cga.empty();
        for (std::sregex_iterator it(cga.begin(), cga.end(), kReGroup), end; it != end && dashOnly; ++it)
            dashOnly = (*it)[1].str() == "Collision_Dash";
        r.real = !dashOnly && (!cga.empty() || paNum > 0 || row.FName(sOverride) != "None" || isWave);
        std::string assist = row.FName(sAssist);
        r.reach = std::regex_search(assist, m, kReReach) ? strtod(m[1].str().c_str(), nullptr) / 100 : 0;
        r.mash = row.FName(sBreak) != "None";
        index[name] = (int)rows.size();
        rows.push_back(std::move(r));
    }
    if (rows.empty()) return false;
    out = Fmt("#table=0x%llX\n#version=7\n", (unsigned long long)st.addr);
    for (size_t i = 0; i < rows.size(); i++) {
        const R_& r = rows[i];
        auto nx = index.find(r.next);
        out += Fmt("%d\t%s\t%d\t%.4f\t%d\t%d\t%d\t%.4f\t%d\t%.4f\t%.4f\t%.1f\t%d\t%.1f\t%.1f\t%d\t%.1f\n", (int)i, r.name.c_str(), r.type, r.dur,
                   nx != index.end() ? nx->second : -1, r.jp, r.ja, r.delay, r.ck, r.cs, r.cl, r.cr, r.mash, r.speed, r.reach, r.real, r.ahead);
    }
    return true;
}

// ---------------------------------------------------------------- live.txt

static std::map<uint64_t, int> g_groggyCls; // 类 -> IsGroggy 偏移（-1 没有）

static int PropOffset(const std::string& cls, const std::string& name) {
    ue::Prop p = ue::FindProp(cls, name);
    return p ? p.off : 0;
}

static bool OuterNamed(uint64_t o, const char* name) { return o && ue::ObjName(o) == name; }

static bool ExportLive(std::string& out, uint64_t& pcOut) {
    using namespace ue;
    uint64_t pcCls = FindObject("/Script/Engine.PlayerController");
    uint64_t pc = FirstInstance(pcCls);
    pcOut = pc;
    if (!pc) return false;
    out = Fmt("pc=0x%llX\n", (unsigned long long)pc);
    if (uint64_t ws = FirstInstance(FindObject("/Script/Engine.WorldSettings")))
        out += Fmt("ws=0x%llX dil=0x%X\n", (unsigned long long)ws, PropOffset("/Script/Engine.WorldSettings", "TimeDilation"));
    // 可惩戒：各敌人类的 IsGroggy
    for (uint64_t a : Instances(FindObject("/Script/SB.SBCharacter"))) {
        uint64_t c = R<uint64_t>(a + 0x10);
        if (g_groggyCls.count(c)) continue;
        Prop p = FindProp(c, "IsGroggy");
        g_groggyCls[c] = p ? p.off : -1;
    }
    std::vector<std::string> parts;
    for (auto& [c, off] : g_groggyCls)
        if (off >= 0) parts.push_back(Fmt("0x%llX:0x%X", (unsigned long long)c, off));
    std::sort(parts.begin(), parts.end());
    std::string joined;
    for (size_t i = 0; i < parts.size(); i++) joined += (i ? "," : "") + parts[i];
    out += Fmt("groggy=%s weak=0x%X\n", joined.c_str(), PropOffset("/Script/SB.SBCharacter", "bActiveWeakPointCollision"));
    // 伊芙血条：WB_MainHUD_PlayerInfo.WidgetTree.ProgressBar_HP（多个取最后一个）
    uint64_t hp = 0;
    for (uint64_t w : Instances(FindObject("/Script/UMG.ProgressBar"))) {
        if (ObjName(w) != "ProgressBar_HP") continue;
        uint64_t tree = Outer(w);
        if (OuterNamed(tree, "WidgetTree") && OuterNamed(Outer(tree), "WB_MainHUD_PlayerInfo")) hp = w;
    }
    if (hp) out += Fmt("hp=0x%llX pct=0x%X\n", (unsigned long long)hp, PropOffset("/Script/UMG.ProgressBar", "Percent"));
    // 过场 QTE 控件（Transient 下的那个）
    uint64_t qte = 0;
    for (uint64_t w : Instances(FindObject("/Script/SB.SBSequencerQTEWidget"))) {
        bool transient = false;
        for (uint64_t o = w; o && !transient; o = Outer(o)) transient = ObjName(o).find("Transient") != std::string::npos;
        if (transient) qte = w;
    }
    if (qte) {
        const std::string W = "/Script/SB.SBSequencerQTEWidget";
        out += Fmt("qte=0x%llX vis=0x%X type=0x%X action=0x%X uiaction=0x%X bind=0x%X\n", (unsigned long long)qte,
                   PropOffset("/Script/UMG.Widget", "Visibility"), PropOffset(W, "InputType"), PropOffset(W, "InputAction"),
                   PropOffset(W, "UIInputAction"), PropOffset(W, "bBindInput"));
    }
    return true;
}

// ---------------------------------------------------------------- projectiles.txt

struct ProjRow { bool jp, ja; double v0, v1; };
static std::map<std::string, ProjRow> g_projRows;
static std::set<uint64_t> g_projKnown;
static uint64_t g_projPC;

static std::string ProjLine(uint64_t o) {
    std::string name = ue::ObjName(ue::R<uint64_t>(o + 0x10));
    if (name.size() > 2 && name.compare(name.size() - 2, 2, "_C") == 0) name.resize(name.size() - 2);
    if (!name.rfind("P_", 0)) return ""; // 伊芙自己的子弹
    auto r = g_projRows.find(name);
    if (r == g_projRows.end()) return "";
    return Fmt("0x%llX %s %d %d %.0f %.0f\n", (unsigned long long)o, name.c_str(), r->second.jp, r->second.ja, r->second.v0, r->second.v1);
}

static void ExportProjectiles(uint64_t pc) {
    using namespace ue;
    if (!pc) return;
    if (g_projRows.empty()) {
        Table pt;
        if (!ReadTable("/Game/Local/Data/ProjectileTable.ProjectileTable", pt)) return;
        Prop pJp = FindProp(pt.rowStruct, "AvailableJustParry"), pJa = FindProp(pt.rowStruct, "AvailableJustAction"),
             pSpeed = FindProp(pt.rowStruct, "Speed"), pMin = FindProp(pt.rowStruct, "MinSpeed"), pMax = FindProp(pt.rowStruct, "MaxSpeed"),
             pAcc = FindProp(pt.rowStruct, "Accelation");
        Row row;
        for (auto& [name, addr] : pt.rows) {
            if (!row.Load(addr, pt.rowSize)) continue;
            double v = row.Float(pSpeed), lo = row.Float(pMin), hi = row.Float(pMax);
            if (lo > 0 && v < lo) v = lo;
            if (hi > 0 && v > hi) v = hi;
            g_projRows[name] = {row.Bool(pJp), row.Bool(pJa), v, row.Float(pAcc) > 0 ? hi : v};
        }
        if (g_projRows.empty()) return;
    }
    std::vector<uint64_t> all = Instances(FindObject("/Script/SB.SBProjectile"));
    std::string add;
    if (pc != g_projPC) {
        g_projPC = pc;
        g_projKnown.clear();
        add = Fmt("#gen=0x%llX\n", (unsigned long long)pc);
    }
    for (uint64_t o : all)
        if (g_projKnown.insert(o).second) add += ProjLine(o);
    if (add.empty()) return;
    std::string cur;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        cur = g_text[(int)BridgeFile::Projectiles];
    }
    Publish(BridgeFile::Projectiles, add.rfind("#gen=", 0) == 0 ? add : cur + add);
}

// ---------------------------------------------------------------- keys.txt

static bool ExportKeys(std::string& out) {
    using namespace ue;
    uint64_t s = FindObject("/Script/Engine.Default__InputSettings");
    uint64_t cls = s ? R<uint64_t>(s + 0x10) : 0;
    if (!cls) return false;
    Prop am = FindProp(cls, "ActionMappings"), xm = FindProp(cls, "AxisMappings");
    if (!am.inner || !xm.inner) return false;
    uint64_t as = R<uint64_t>(am.inner + 0x78), xs = R<uint64_t>(xm.inner + 0x78);
    int asz = R<int32_t>(am.inner + 0x3C), xsz = R<int32_t>(xm.inner + 0x3C);
    Prop aName = FindProp(as, "ActionName"), aKey = FindProp(as, "Key"), aShift = FindProp(as, "bShift"), aCtrl = FindProp(as, "bCtrl"),
         aAlt = FindProp(as, "bAlt"), aCmd = FindProp(as, "bCmd");
    Prop xName = FindProp(xs, "AxisName"), xKey = FindProp(xs, "Key"), xScale = FindProp(xs, "Scale");
    Prop keyName = FindProp("/Script/InputCore.Key", "KeyName");
    if (!aName || !aKey || !aShift || !aCtrl || !aAlt || !aCmd || !xName || !xKey || !xScale || !keyName || asz <= 0 || xsz <= 0) return false;
    auto readArr = [&](const Prop& p, int esz, std::vector<Row>& rows) {
        struct { uint64_t data; int32_t num, max; } a{};
        if (!Read(s + p.off, &a, sizeof(a)) || a.num < 0 || a.num > 4096) return false;
        rows.resize(a.num);
        for (int i = 0; i < a.num; i++)
            if (!rows[i].Load(a.data + (uint64_t)i * esz, esz)) return false;
        return true;
    };
    std::vector<Row> acts, axes;
    if (!readArr(am, asz, acts) || !readArr(xm, xsz, axes)) return false;
    auto key = [&](const Row& r, const Prop& k) {
        Prop kn = keyName;
        kn.off += k.off;
        return r.FName(kn);
    };
    static const char* const kActions[] = {"Guard", "Evade", "AttackLight", "AttackStrong", "Jump", "Interaction_Key"};
    std::vector<std::string> lines;
    for (const char* a : kActions) {
        std::string ks;
        // 顺序同 Bridge 用的 GetActionMappingByName：数组倒序
        for (auto it = acts.rbegin(); it != acts.rend(); ++it) {
            const Row& r = *it;
            if (r.FName(aName) != a) continue;
            std::string k = key(r, aKey);
            if (k != "None" && !r.Bool(aShift) && !r.Bool(aCtrl) && !r.Bool(aAlt) && !r.Bool(aCmd)) ks += (ks.empty() ? "" : ",") + k;
        }
        lines.push_back(std::string(a) + "=" + ks);
    }
    for (const char* a : {"MoveForward", "MoveRight"}) {
        std::string ks;
        for (auto it = axes.rbegin(); it != axes.rend(); ++it) {
            const Row& r = *it;
            if (r.FName(xName) != a) continue;
            std::string k = key(r, xKey);
            if (k != "None") ks += (ks.empty() ? "" : ",") + Fmt("%s:%g", k.c_str(), (double)r.Float(xScale));
        }
        lines.push_back(std::string(a) + "=" + ks);
    }
    std::string names;
    for (const char* a : kActions)
        for (const std::string& n : {std::string(a), std::string("UI_QTE_") + a})
            if (uint32_t idx = FindName(n)) names += (names.empty() ? "" : ",") + Fmt("%u:%s", idx, a);
    lines.push_back("@names=" + names);
    out.clear();
    for (size_t i = 0; i < lines.size(); i++) out += lines[i] + "\n";
    return true;
}

// ---------------------------------------------------------------- 线程

static volatile LONG g_stepsOk;

static DWORD WINAPI Worker(void*) {
    if (!ue::Open(g_pid, g_objArray, g_namePool)) return 0;
    {
        // FindObject 路径里的包名 / 对象名、键位动作名
        std::vector<std::string> want = {"/Script/Engine", "/Script/SB", "/Script/UMG", "/Script/InputCore", "PlayerController", "WorldSettings",
                                         "SBCharacter", "ProgressBar", "SBSequencerQTEWidget", "SBProjectile", "Widget", "Key",
                                         "Default__InputSettings"};
        for (const char* t : {"SkillActiveStepTable", "EffectTable", "ProjectileTable", "TargetFilterTable"})
            want.push_back(std::string("/Game/Local/Data/") + t), want.push_back(t);
        for (const char* a : {"Guard", "Evade", "AttackLight", "AttackStrong", "Jump", "Interaction_Key"})
            want.push_back(a), want.push_back(std::string("UI_QTE_") + a);
        ue::WantNames(want);
    }
    g_groggyCls.clear();
    g_projRows.clear();
    g_projKnown.clear();
    g_projPC = 0;
    ULONGLONG lastLive = 0, lastKeys = 0, lastSteps = 0;
    uint64_t pc = 0;
    while (!g_stop) {
        ue::RefreshObjects();
        ULONGLONG now = GetTickCount64();
        std::string s;
        if (!g_stepsOk && now - lastSteps >= 2000) {
            lastSteps = now;
            if (ExportSteps(s)) { Publish(BridgeFile::Steps, s); InterlockedExchange(&g_stepsOk, 1); }
        }
        if (now - lastLive >= 1000) {
            lastLive = now;
            if (ExportLive(s, pc)) Publish(BridgeFile::Live, s);
        }
        ExportProjectiles(pc);
        if (now - lastKeys >= 3000) {
            lastKeys = now;
            if (ExportKeys(s)) Publish(BridgeFile::Keys, s);
        }
        for (int i = 0; i < 4 && !g_stop; i++) Sleep(50);
    }
    ue::Close();
    return 0;
}

void NativeStart(DWORD pid, uint64_t objArray, uint64_t namePool) {
    NativeStop();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& t : g_text) t.clear();
    }
    g_pid = pid, g_objArray = objArray, g_namePool = namePool;
    g_stop = 0;
    g_stepsOk = 0;
    g_startedAt = GetTickCount64();
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

void NativeStop() {
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    if (GetThreadId(g_thread) != GetCurrentThreadId()) WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& t : g_text) t.clear();
    g_stepsOk = 0;
}

bool NativeActive() { return g_stepsOk != 0; }

// ---------------------------------------------------------------- 读取方

TextState BridgeText(BridgeFile which, std::string& text, uint64_t& stamp) {
    const int i = (int)which;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_text[i].empty()) {
            uint64_t st = (1ull << 63) | g_gen[i];
            if (st == stamp) return TextState::Unchanged;
            text = g_text[i];
            stamp = st;
            return TextState::Changed;
        }
    }
    if (g_cfg.dataSource == DataSource::Native) return TextState::Missing; // 只用原生
    if (g_thread && GetTickCount64() - g_startedAt < kWaitNativeMs) return TextState::Missing;
    // 退回读 Bridge 写的文件
    wchar_t name[32];
    swprintf(name, 32, L"\\%hs", kFileNames[i]);
    std::wstring path = BridgeDir() + name;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return TextState::Missing;
    if (CompareFileTime(&fa.ftLastWriteTime, &GameStartTime()) < 0) return TextState::Stale;
    uint64_t ft = ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    uint64_t size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    uint64_t st = (ft + size * 0x9E3779B97F4A7C15ull) & ~(1ull << 63);
    if (st == stamp) return TextState::Unchanged;
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return TextState::Unchanged; // Bridge 正在写：下次再读
    std::string s((size_t)size, '\0');
    size_t got = fread(s.data(), 1, s.size(), f);
    fclose(f);
    s.resize(got);
    text.swap(s);
    stamp = st;
    return TextState::Changed;
}

bool NextLine(const std::string& s, size_t& pos, char* buf, size_t n) {
    if (pos >= s.size() || !n) return false;
    size_t e = s.find('\n', pos);
    if (e == std::string::npos) e = s.size();
    size_t len = std::min(e - pos, n - 1);
    memcpy(buf, s.data() + pos, len);
    buf[len] = 0;
    if (len && buf[len - 1] == '\r') buf[len - 1] = 0;
    pos = e + 1;
    return true;
}
