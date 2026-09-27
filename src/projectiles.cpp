#include "projectiles.h"
#include "game.h"
#include "live.h"

// 对象池里的道具不飞时停在原点（或停在上次消失的位置），位置在变才算“在飞”
static const double kActiveGap = 0.06;  // 超过这么久位置没变就当已停
// 命中时刻：道具中心进入伊芙 kHitRadius 以内。实测渡鸦剑气的完美闪避在中心离伊芙 1.07m 时结算
static const double kHitRadius = 105;
static const double kMissRadius = 160;  // 最近距离超过这个就当打不到（宽剑气、贴身擦过的留点余量）
static const double kMaxAhead = 2.0;    // 只看 2 秒内会到的

struct Proj {
    bool jp = false, ja = false;
    float vInit = 0, vMax = 0; // 道具表速度（厘米/秒）：刚发射时测出的速度偏低（取样窗口里有还没动起来的几帧），用它兜底
    float pos[3]{};
    uint64_t movedTsc = 0; // 上次位置变化
    double vel[3]{};       // 厘米/秒
    bool hasVel = false;
    // 最近约 0.15 秒的位置样本：我们读内存的时刻和游戏帧不对齐，两次读之间可能隔 0 帧、1 帧或 2 帧，
    // 只用相邻两次算速度会忽大忽小（判定条上的点会乱跳）。用最早和最新的样本算；刚发射时跨度还短，速度大小靠道具表兜底
    struct Sample { uint64_t tsc; float pos[3]; };
    std::deque<Sample> hist;
};
static std::map<uint64_t, Proj> g_projs;
static uint64_t g_fileSize = ~0ull;
static FILETIME g_fileTime;
static std::string g_fileGen; // 首行 "#gen=..."：Bridge 每次全量重写（开局 / 换关卡）都换一个
static ULONGLONG g_lastCheck;

void ResetProjectiles() {
    g_projs.clear();
    g_fileSize = ~0ull;
    g_fileTime = {};
    g_fileGen.clear();
}

// Bridge 写的 projectiles.txt：首行 "#gen=..."，之后每行“0x地址 行名 jp ja 初速 最高速”；开局全量，之后新建的池对象追加
static void Reload() {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastCheck < 100) return;
    g_lastCheck = now;
    std::wstring path = BridgeDir() + L"\\projectiles.txt";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return;
    uint64_t size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (size == g_fileSize && !CompareFileTime(&fa.ftLastWriteTime, &g_fileTime)) return;
    // 上一局游戏留下的文件：地址对不上，不读
    if (CompareFileTime(&fa.ftLastWriteTime, &GameStartTime()) < 0) return;
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return;
    char line[256], name[160];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "#gen=", 5)) { fclose(f); return; } // 正在重写
    g_fileSize = size, g_fileTime = fa.ftLastWriteTime;
    // 换了一代 = 换关卡重写了：旧地址全部作废
    if (g_fileGen != line) g_projs.clear(), g_fileGen = line;
    uint64_t addr;
    int jp, ja;
    while (fgets(line, sizeof(line), f)) {
        float v0 = 0, v1 = 0;
        if (sscanf_s(line, "0x%llx %159s %d %d %f %f", &addr, name, (unsigned)sizeof(name), &jp, &ja, &v0, &v1) < 4) continue;
        Proj& p = g_projs[addr];
        p.jp = jp != 0, p.ja = ja != 0, p.vInit = v0, p.vMax = v1;
    }
    fclose(f);
}

// 相对位置 p、速度 v：最早 |p + v t| = R 的 t；到不了返回 -1
static double Contact(const double p[3], const double v[3], double R) {
    double a = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    double b = 2 * (p[0] * v[0] + p[1] * v[1] + p[2] * v[2]);
    double c = p[0] * p[0] + p[1] * p[1] + p[2] * p[2] - R * R;
    if (c <= 0) return 0;
    double disc = b * b - 4 * a * c;
    if (a < 1e-3 || disc < 0) return -1;
    double t = (-b - std::sqrt(disc)) / (2 * a);
    return t >= 0 ? t : -1;
}

// 相对位置 p、速度 v：离伊芙最近的时刻 t（已经飞过去了返回 -1）和那时的距离
static double Closest(const double p[3], const double v[3], double& dmin) {
    double a = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (a < 1e-3) return -1;
    double t = -(p[0] * v[0] + p[1] * v[1] + p[2] * v[2]) / a;
    if (t < 0) return -1;
    double d2 = 0;
    for (int i = 0; i < 3; i++) d2 += (p[i] + v[i] * t) * (p[i] + v[i] * t);
    dmin = std::sqrt(d2);
    return t;
}

bool ProjectilesTick(std::vector<ProjHit>& out) {
    out.clear();
    if (!g.ok) return false;
    Reload();
    float eve[3];
    if (g_projs.empty() || !ActorLocation(g_live.player, eve)) return false;
    bool flying = false;
    uint64_t now = __rdtsc();
    for (auto& [addr, p] : g_projs) {
        float pos[3];
        if (!ActorLocation(addr, pos)) continue;
        if (std::fabs(pos[0]) < 1 && std::fabs(pos[1]) < 1 && std::fabs(pos[2]) < 1) { p.hasVel = false; continue; }
        if (memcmp(pos, p.pos, sizeof(pos)) != 0) {
            // 刚从池里取出（上次移动很久以前）：旧样本作废
            if (!p.movedTsc || now - p.movedTsc > SecToTsc(0.2)) p.hist.clear();
            Proj::Sample smp{now, {pos[0], pos[1], pos[2]}};
            p.hist.push_back(smp);
            while (p.hist.size() > 2 && now - p.hist[1].tsc > SecToTsc(0.15)) p.hist.pop_front();
            const Proj::Sample& o = p.hist.front();
            double dt = TscToSec((int64_t)(now - o.tsc));
            p.hasVel = p.hist.size() >= 2 && dt > 0.001;
            if (p.hasVel)
                for (int i = 0; i < 3; i++) p.vel[i] = (pos[i] - o.pos[i]) / dt;
            memcpy(p.pos, pos, sizeof(pos));
            p.movedTsc = now;
        }
        if (!p.hasVel || TscToSec((int64_t)(now - p.movedTsc)) > kActiveGap) continue;
        flying = true;
        // 从上次取样到现在又飞了一段
        // 方向用实测，速度大小夹到道具表的 [初速, 最高速] 里
        double v[3] = {p.vel[0], p.vel[1], p.vel[2]};
        double sp = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        double want = std::max<double>(sp, p.vInit);
        if (p.vMax > 0) want = std::min<double>(want, p.vMax);
        if (sp > 1 && want > 0)
            for (double& x : v) x *= want / sp;
        sp = want > 0 ? want : sp;
        double since = TscToSec((int64_t)(now - p.movedTsc));
        double rel[3];
        for (int i = 0; i < 3; i++) rel[i] = p.pos[i] + v[i] * since - eve[i];
        double dmin = 0, tc = Closest(rel, v, dmin);
        if (tc < 0 || tc > kMaxAhead || dmin > kMissRadius) continue;
        double t = Contact(rel, v, kHitRadius);
        if (t < 0) t = tc; // 最近距离在 1.05~1.6m：按最近点
        double d = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        out.push_back({addr, t, p.jp, p.ja, (float)(sp / 100), (float)(dmin / 100), (float)(d / 100)});
    }
    return flying;
}
