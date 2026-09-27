#include "live.h"
#include "game.h"

Live g_live;
HWND g_gameWnd;

// 以下偏移都由 UE4SS 反射查得
static const uint64_t kActorRoot = 0x178, kCompLoc = 0x13C, kPcmPov = 0x1CE0;
static const uint64_t kPcPawn = 0x2F8, kPcCameraManager = 0x360;
static const uint64_t kCharLockOn = 0x1438, kCharCameraLookAt = 0x1460; // SBCharacter.LockOnCharacter / CameraLookAtTarget
static const uint64_t kActorCustomTimeDilation = 0xB0;
static uint64_t g_worldSettings, g_worldDilOff;

// 过场 QTE 控件（SBSequencerQTEWidget）的地址和字段偏移，由 SBParryBridge 反射查得
struct QteWidget { uint64_t addr = 0; uint32_t vis = 0, type = 0, action = 0, uiaction = 0, bind = 0; };
static QteWidget g_qteW;
static uint64_t g_hpBar; // 伊芙血条 ProgressBar 及其 Percent 偏移
static uint32_t g_hpPctOff;
static std::map<uint64_t, uint32_t> g_groggyOff; // 敌人蓝图类 -> IsGroggy 偏移（部分 Boss 才有）
static uint32_t g_weakOff;                       // SBCharacter.bActiveWeakPointCollision：没有 IsGroggy 的敌人用它

static void ResetLiveFile() {
    g_live.pc = 0;
    g_qteW = {};
    g_hpBar = 0, g_hpPctOff = 0;
    g_groggyOff.clear();
    g_weakOff = 0;
    g_worldSettings = 0, g_worldDilOff = 0;
}

// SBParryBridge 导出的 live.txt（格式见 bridge 的 exportLive）。打不开（Bridge 正在重写）就沿用上次的值；
// 比游戏进程还旧的是上一局留下的，地址全都不对，不用
static void ReadLiveFile() {
    std::wstring path = BridgeDir() + L"\\live.txt";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &GameStartTime()) < 0) { ResetLiveFile(); return; }
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return;
    ResetLiveFile();
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        sscanf_s(line, "pc=0x%llx", &g_live.pc);
        sscanf_s(line, "hp=0x%llx pct=0x%x", &g_hpBar, &g_hpPctOff);
        if (!strncmp(line, "groggy=", 7)) {
            const char* p = line + 7;
            uint64_t cls;
            uint32_t off;
            int n = 0;
            while (sscanf_s(p, "0x%llx:0x%x%n", &cls, &off, &n) == 2) {
                g_groggyOff[cls] = off;
                p += n;
                if (*p == ',') p++;
            }
            if (const char* w = strstr(line, "weak=0x")) sscanf_s(w, "weak=0x%x", &g_weakOff);
        }
        sscanf_s(line, "ws=0x%llx dil=0x%llx", &g_worldSettings, &g_worldDilOff);
        QteWidget& q = g_qteW;
        sscanf_s(line, "qte=0x%llx vis=0x%x type=0x%x action=0x%x uiaction=0x%x bind=0x%x", &q.addr, &q.vis, &q.type, &q.action,
                 &q.uiaction, &q.bind);
    }
    fclose(f);
}

// 惩戒条件（技能 P_Eve_Sword_Normal_LinkAttack1_1 的目标过滤）：敌人 ActorState_Groggy、3m 内、正前方。
// ActorState 在原生代码里；实测渡鸦倒地可惩戒时蓝图的 IsGroggy 和 bActiveWeakPointCollision 同时 0->1，开始惩戒时变回 0
bool EnemyGroggy() {
    uint64_t e = TargetEnemy(), cls = 0;
    if (!g.ok || !e || !Read(e + 0x10, cls)) return false;
    auto it = g_groggyOff.find(cls);
    uint32_t off = it != g_groggyOff.end() ? it->second : g_weakOff;
    uint8_t b = 0;
    return off && Read(e + off, b) && (b & 1);
}

float EveHpPercent() {
    float p = -1;
    if (!g.ok || !g_hpBar || !g_hpPctOff || !Read(g_hpBar + g_hpPctOff, p) || !(p >= 0 && p <= 1)) return -1;
    return p;
}

float EveTimeScale() {
    float world = 1, self = 1;
    if (g_worldSettings && g_worldDilOff) Read(g_worldSettings + g_worldDilOff, world);
    if (g_live.player) Read(g_live.player + kActorCustomTimeDilation, self);
    float s = world * self;
    return s > 0.001f && s < 10.f ? s : 1.f;
}

int CutsceneQteAction() {
    const QteWidget& q = g_qteW;
    if (!g.ok || !q.addr || !q.action) return -1;
    uint8_t vis = 1, bind = 0;
    uint32_t action = 0, uiaction = 0;
    // ESlateVisibility：0 Visible  1 Collapsed  2 Hidden  3/4 HitTestInvisible（显示中）
    if (!Read(q.addr + q.vis, vis) || !(vis == 0 || vis == 3 || vis == 4)) return -1;
    Read(q.addr + q.bind, bind);
    Read(q.addr + q.action, action);
    Read(q.addr + q.uiaction, uiaction);
    if (!bind && !action && !uiaction) return -1;
    return (int)(action ? action : uiaction);
}

static BOOL CALLBACK EnumGameWindow(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT r;
    if (pid == g.pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && GetClientRect(h, &r) && r.right * r.bottom > 200000) {
        *(HWND*)lp = h;
        return FALSE;
    }
    return TRUE;
}

void LiveTick() {
    static ULONGLONG lastFile = 0, lastWnd = 0;
    ULONGLONG now = GetTickCount64();
    if (!g.ok) {
        if (g_live.pc || g_gameWnd) ResetLiveFile();
        g_live = Live{};
        g_gameWnd = nullptr;
        return;
    }
    if (now - lastFile > 1000) { lastFile = now; ReadLiveFile(); }
    if (now - lastWnd > 2000 && (!g_gameWnd || !IsWindow(g_gameWnd))) {
        lastWnd = now;
        g_gameWnd = nullptr;
        EnumWindows(EnumGameWindow, (LPARAM)&g_gameWnd);
    }
    Live l;
    l.pc = g_live.pc;
    if (l.pc && Read(l.pc + kPcPawn, l.player) && l.player && Read(l.pc + kPcCameraManager, l.pcm)) {
        if (!Read(l.player + kCharLockOn, l.enemy) || !l.enemy) Read(l.player + kCharCameraLookAt, l.enemy);
        if (l.enemy == l.player) l.enemy = 0;
    }
    if (!l.player || !l.pcm) l = Live{l.pc};
    l.lastEnemy = l.enemy ? l.enemy : (l.pc == g_live.pc ? g_live.lastEnemy : 0);
    float tmp[3];
    if (l.lastEnemy && !ActorLocation(l.lastEnemy, tmp)) l.lastEnemy = 0;
    g_live = l;
}

bool GameClient(RECT& r) {
    RECT cr;
    if (!g_gameWnd || !GetClientRect(g_gameWnd, &cr)) return false;
    POINT o{0, 0};
    ClientToScreen(g_gameWnd, &o);
    r = {o.x, o.y, o.x + cr.right, o.y + cr.bottom};
    return cr.right > 0 && cr.bottom > 0;
}

bool GameForeground() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return g.ok && pid == g.pid;
}

bool ActorLocation(uint64_t actor, float out[3]) {
    uint64_t root = 0;
    return actor && Read(actor + kActorRoot, root) && root && Rpm(root + kCompLoc, out, 12);
}

bool CameraPov(float loc[3], float rot[3], float& fov) {
    uint8_t b[0x1C];
    if (!g_live.pcm || !Rpm(g_live.pcm + kPcmPov, b, sizeof(b))) return false;
    memcpy(loc, b, 12);
    memcpy(rot, b + 12, 12);
    memcpy(&fov, b + 24, 4);
    return fov > 5 && fov < 170;
}

// UE：X 前 Y 右 Z 上，FOV 为水平视角
bool Project(const float w[3], int& sx, int& sy) {
    float c[3], rot[3], fov;
    RECT r;
    if (!GameClient(r) || !CameraPov(c, rot, fov)) return false;
    double CP = cos(rot[0] * kDegToRad), SP = sin(rot[0] * kDegToRad), CY = cos(rot[1] * kDegToRad), SY = sin(rot[1] * kDegToRad);
    double CR = cos(rot[2] * kDegToRad), SR = sin(rot[2] * kDegToRad);
    double F[3] = {CP * CY, CP * SY, SP};
    double R[3] = {SR * SP * CY - CR * SY, SR * SP * SY + CR * CY, -SR * CP};
    double U[3] = {-(CR * SP * CY + SR * SY), CY * SR - CR * SP * SY, CR * CP};
    double d[3] = {w[0] - c[0], w[1] - c[1], w[2] - c[2]};
    double z = d[0] * F[0] + d[1] * F[1] + d[2] * F[2];
    if (z < 10) return false;
    double x = d[0] * R[0] + d[1] * R[1] + d[2] * R[2], y = d[0] * U[0] + d[1] * U[1] + d[2] * U[2];
    double W = r.right - r.left, H = r.bottom - r.top, f = (W / 2) / tan(fov * kDegToRad / 2);
    sx = r.left + (int)(W / 2 + x / z * f);
    sy = r.top + (int)(H / 2 - y / z * f);
    return true;
}

uint64_t TargetEnemy() { return g_live.enemy ? g_live.enemy : g_live.lastEnemy; }

float TargetDistance() {
    float a[3], b[3];
    if (!ActorLocation(g_live.player, a) || !ActorLocation(TargetEnemy(), b)) return -1;
    return (float)std::hypot(a[0] - b[0], a[1] - b[1]) / 100.f;
}
