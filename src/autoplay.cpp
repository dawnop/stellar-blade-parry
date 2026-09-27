#include "autoplay.h"
#include "config.h"
#include "game.h"
#include "live.h"
#include "tracker.h"

// ---------------------------------------------------------------- 键位

// Guard..Interact 是按钮（手柄上也是按钮），Fwd..Right 是移动方向（键盘 WASD / 手柄左摇杆）
enum class Btn { Guard, Evade, Light, Strong, Jump, Interact, Fwd, Back, Left, Right, Count };

// 一个按键：键盘/鼠标（vk）或手柄（XInput 位 + ScePad 位）
struct Key {
    WORD vk = 0;
    bool mouse = false;
    uint16_t xi = 0;
    uint32_t ps = 0;
};
struct Binding {
    Key kb;     // 键盘或鼠标按键
    Key pad;    // 手柄按键（摇杆方向不在这里）
};
static Binding g_bind[(int)Btn::Count];
static std::map<uint32_t, Btn> g_actionByName; // 动作名 FName 序号 -> 按钮（过场 QTE 用）

// UE 键名 -> 虚拟键码
static WORD VkFromUeName(const std::string& n) {
    if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return (WORD)n[0];
    static const std::map<std::string, WORD> m = {
        {"LeftShift", VK_LSHIFT}, {"RightShift", VK_RSHIFT}, {"LeftControl", VK_LCONTROL}, {"RightControl", VK_RCONTROL},
        {"LeftAlt", VK_LMENU}, {"RightAlt", VK_RMENU}, {"SpaceBar", VK_SPACE}, {"Tab", VK_TAB}, {"CapsLock", VK_CAPITAL},
        {"Enter", VK_RETURN}, {"BackSpace", VK_BACK}, {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
        {"Zero", '0'}, {"One", '1'}, {"Two", '2'}, {"Three", '3'}, {"Four", '4'}, {"Five", '5'}, {"Six", '6'}, {"Seven", '7'},
        {"Eight", '8'}, {"Nine", '9'}, {"LeftMouseButton", VK_LBUTTON}, {"RightMouseButton", VK_RBUTTON},
        {"MiddleMouseButton", VK_MBUTTON}, {"ThumbMouseButton", VK_XBUTTON1}, {"ThumbMouseButton2", VK_XBUTTON2},
        {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
        {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
        {"Semicolon", VK_OEM_1}, {"Comma", VK_OEM_COMMA}, {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2}, {"Tilde", VK_OEM_3},
    };
    auto it = m.find(n);
    return it == m.end() ? 0 : it->second;
}

// UE 手柄键名 -> XInput / ScePad 位（扳机是模拟量，不支持）
static bool PadFromUeName(const std::string& n, uint16_t& xi, uint32_t& ps) {
    static const std::map<std::string, std::pair<uint16_t, uint32_t>> m = {
        {"Gamepad_LeftShoulder", {0x0100, 0x0400}},  {"Gamepad_RightShoulder", {0x0200, 0x0800}},
        {"Gamepad_FaceButton_Bottom", {0x1000, 0x4000}}, {"Gamepad_FaceButton_Right", {0x2000, 0x2000}},
        {"Gamepad_FaceButton_Left", {0x4000, 0x8000}},   {"Gamepad_FaceButton_Top", {0x8000, 0x1000}},
        {"Gamepad_LeftThumbstick", {0x0040, 0x0002}},    {"Gamepad_RightThumbstick", {0x0080, 0x0004}},
    };
    auto it = m.find(n);
    if (it == m.end()) return false;
    xi = it->second.first;
    ps = it->second.second;
    return true;
}

static void SetDefaults() {
    // 游戏默认键位（keys.txt 还没导出时用）
    g_bind[(int)Btn::Guard] = {{'E'}, {0, false, 0x0100, 0x0400}};
    g_bind[(int)Btn::Evade] = {{VK_LSHIFT}, {0, false, 0x2000, 0x2000}};
    g_bind[(int)Btn::Light] = {{VK_LBUTTON, true}, {0, false, 0x4000, 0x8000}};
    g_bind[(int)Btn::Strong] = {{VK_RBUTTON, true}, {0, false, 0x8000, 0x1000}};
    g_bind[(int)Btn::Jump] = {{VK_SPACE}, {0, false, 0x1000, 0x4000}};
    g_bind[(int)Btn::Interact] = {{'F'}, {}};
    g_bind[(int)Btn::Fwd] = {{'W'}, {}};
    g_bind[(int)Btn::Back] = {{'S'}, {}};
    g_bind[(int)Btn::Left] = {{'A'}, {}};
    g_bind[(int)Btn::Right] = {{'D'}, {}};
}

// keys.txt：Guard=E,ThumbMouseButton,Gamepad_LeftShoulder / MoveForward=W:1,S:-1,Gamepad_LeftY:1
static void LoadKeys() {
    static ULONGLONG last = 0;
    static FILETIME lastWrite{};
    ULONGLONG now = GetTickCount64();
    if (last && now - last < 3000) return;
    last = now;
    if (!g_bind[0].kb.vk && !g_bind[0].kb.mouse) SetDefaults();
    std::wstring path = BridgeDir() + L"\\keys.txt";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) || !CompareFileTime(&fa.ftLastWriteTime, &lastWrite)) return;
    lastWrite = fa.ftLastWriteTime;
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return;
    // 文件里出现的动作按文件重建（改了键的旧键位不能留着），没列出的键 / 手柄键用默认值
    SetDefaults();
    Binding def[(int)Btn::Count];
    std::copy(std::begin(g_bind), std::end(g_bind), def);
    auto fallback = [&](Btn b) {
        Binding& x = g_bind[(int)b];
        if (!x.kb.vk) x.kb = def[(int)b].kb;
        if (!x.pad.xi && !x.pad.ps) x.pad = def[(int)b].pad;
    };
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string name = s.substr(0, eq), list = s.substr(eq + 1);
        auto btnOf = [](const std::string& a) {
            return a == "Guard" ? Btn::Guard : a == "Evade" ? Btn::Evade : a == "AttackLight" ? Btn::Light : a == "AttackStrong" ? Btn::Strong
                 : a == "Jump" ? Btn::Jump : a == "Interaction_Key" ? Btn::Interact : Btn::Count;
        };
        if (name == "@names") {
            // 19454016:AttackLight,...
            g_actionByName.clear();
            for (size_t p = 0; p < list.size();) {
                size_t c = list.find(',', p);
                if (c == std::string::npos) c = list.size();
                std::string item = list.substr(p, c - p);
                size_t colon = item.find(':');
                Btn b = colon == std::string::npos ? Btn::Count : btnOf(item.substr(colon + 1));
                if (b != Btn::Count) g_actionByName[(uint32_t)strtoul(item.c_str(), nullptr, 10)] = b;
                p = c + 1;
            }
            continue;
        }
        bool axis = name == "MoveForward" || name == "MoveRight";
        Btn pos = axis ? (name == "MoveForward" ? Btn::Fwd : Btn::Right) : btnOf(name);
        if (pos == Btn::Count) continue;
        Btn neg = name == "MoveForward" ? Btn::Back : Btn::Left;
        g_bind[(int)pos] = {};
        if (axis) g_bind[(int)neg] = {};
        bool gotKb[2] = {}, gotPad = false;
        for (size_t p = 0; p <= list.size();) {
            size_t c = list.find(',', p);
            if (c == std::string::npos) c = list.size();
            std::string item = list.substr(p, c - p);
            p = c + 1;
            if (item.empty()) continue;
            float scale = 1;
            if (axis) {
                size_t colon = item.find(':');
                if (colon != std::string::npos) { scale = (float)atof(item.c_str() + colon + 1); item.resize(colon); }
            }
            Binding& b = g_bind[(int)(scale < 0 ? neg : pos)];
            int slot = scale < 0 ? 1 : 0;
            uint16_t xi;
            uint32_t ps;
            if (!axis && !gotPad && PadFromUeName(item, xi, ps)) { b.pad = {0, false, xi, ps}; gotPad = true; continue; }
            WORD vk = VkFromUeName(item);
            // 移动轴同时绑了 QWERTY 和 AZERTY（Z/Q 在前），优先 WASD：Q 还是本程序的退出快捷键
            if (axis && vk && (vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D')) { b.kb = {vk, false}; gotKb[slot] = true; continue; }
            if (vk && !gotKb[slot]) {
                // 优先键盘键，鼠标键备用
                bool mouse = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
                if (!mouse || !b.kb.vk) b.kb = {vk, mouse};
                if (!mouse) gotKb[slot] = true;
            }
        }
        fallback(pos);
        if (axis) fallback(neg);
    }
    fclose(f);
    Log(TR("[SBParry] 已读取游戏键位\n", "[SBParry] Game key bindings loaded\n"));
}

// ---------------------------------------------------------------- 输入后端

static InputDevice g_dev = InputDevice::Keyboard;
static ULONGLONG g_kbRealAt; // 玩家真实操作键鼠的最后时刻（不含本程序 SendInput 的）

// Raw Input 里 SendInput 模拟的事件 hDevice 为空，据此只统计真实键鼠；按着 Ctrl/Alt 的按键是在用快捷键，也不算
void AutoInit(HWND w) {
    RAWINPUTDEVICE rid[2] = {{0x01, 0x06, RIDEV_INPUTSINK, w}, {0x01, 0x02, RIDEV_INPUTSINK, w}}; // 键盘、鼠标
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
}

void AutoOnRawInput(LPARAM lp) {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1 || !ri.header.hDevice) return;
    if (ri.header.dwType == RIM_TYPEKEYBOARD && ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000)) return;
    if (ri.header.dwType == RIM_TYPEMOUSE && !ri.data.mouse.usButtonFlags && std::abs(ri.data.mouse.lLastX) + std::abs(ri.data.mouse.lLastY) < 3)
        return; // 鼠标轻微抖动不算
    g_kbRealAt = GetTickCount64();
}

static InputDevice PickDevice() {
    if (g_cfg.autoDevice != InputDevice::Auto) return g_cfg.autoDevice;
    PadCtrl c{};
    if (!g.ctrl || !Rpm(g.ctrl, &c, sizeof(c))) return InputDevice::Keyboard;
    // 键鼠最近一次真实操作 vs 手柄钩子记录的最近一次真实操作（都不含本程序模拟的）
    double kbAgo = g_kbRealAt ? (GetTickCount64() - g_kbRealAt) / 1000.0 : 1e9;
    // 还没见过任何真实操作（刚启动就挂机）：接了 Xbox 手柄就用手柄注入（在游戏内叠加，比模拟键盘可靠）
    if (!g_kbRealAt && !c.xiLastReal && !c.psLastReal) return c.xiCalls ? InputDevice::XInput : InputDevice::Keyboard;
    uint64_t now = __rdtsc();
    double xiAgo = c.xiLastReal ? TscToSec((int64_t)(now - c.xiLastReal)) : 1e9;
    double psAgo = c.psLastReal ? TscToSec((int64_t)(now - c.psLastReal)) : 1e9;
    if (xiAgo < kbAgo && xiAgo <= psAgo) return InputDevice::XInput;
    if (psAgo < kbAgo) return InputDevice::DualSense;
    return InputDevice::Keyboard;
}

const wchar_t* AutoDeviceName() {
    switch (g_dev) {
    case InputDevice::XInput: return L"XInput";
    case InputDevice::DualSense: return L"DualSense";
    default: return TR("键盘", "Keyboard");
    }
}

static void SendKey(const Key& k, bool down) {
    INPUT in{};
    if (k.mouse) {
        in.type = INPUT_MOUSE;
        switch (k.vk) {
        case VK_LBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case VK_RBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case VK_MBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        default:
            in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            in.mi.mouseData = k.vk == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2;
        }
    } else {
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = k.vk;
        in.ki.wScan = (WORD)MapVirtualKeyW(k.vk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
        if (k.vk == VK_UP || k.vk == VK_DOWN || k.vk == VK_LEFT || k.vk == VK_RIGHT || k.vk == VK_RCONTROL || k.vk == VK_RMENU ||
            k.vk == VK_INSERT || k.vk == VK_DELETE || k.vk == VK_HOME || k.vk == VK_END || k.vk == VK_PRIOR || k.vk == VK_NEXT)
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(1, &in, sizeof(in));
}

// 当前要按住的东西：按钮集合 + 可选的摇杆方向（相对相机的角度，度，0=前 90=右）
struct Hold {
    bool btn[(int)Btn::Count] = {};
    bool stick = false;
    double angle = 0;
};
static Hold g_applied; // 已经发给游戏的状态
static InputDevice g_appliedDev = InputDevice::Keyboard;

static void Apply(const Hold& want) {
    g_appliedDev = g_dev;
    if (g_dev == InputDevice::Keyboard) {
        Hold w = want;
        if (w.stick) {
            // 键盘只有 8 个方向，取最近的
            double c = cos(w.angle * kDegToRad), s = sin(w.angle * kDegToRad);
            w.btn[(int)Btn::Fwd] = c > 0.38;
            w.btn[(int)Btn::Back] = c < -0.38;
            w.btn[(int)Btn::Right] = s > 0.38;
            w.btn[(int)Btn::Left] = s < -0.38;
        }
        for (int i = 0; i < (int)Btn::Count; i++)
            if (w.btn[i] != g_applied.btn[i]) SendKey(g_bind[i].kb, w.btn[i]);
        g_applied = w;
        g_applied.stick = false;
        return;
    }
    if (!g.ok || !g.ctrl) return;
    PadCtrl c{};
    bool xi = g_dev == InputDevice::XInput;
    uint32_t mask = 0;
    for (int i = 0; i <= (int)Btn::Interact; i++)
        if (want.btn[i]) mask |= xi ? g_bind[i].pad.xi : g_bind[i].pad.ps;
    double a = want.angle * kDegToRad;
    if (xi) {
        c.xiActive = mask || want.stick;
        c.xiButtons = (uint16_t)mask;
        c.xiStick = want.stick;
        c.xiLX = (int16_t)(sin(a) * 32000);
        c.xiLY = (int16_t)(cos(a) * 32000);
        WriteData(g.ctrl, &c, offsetof(PadCtrl, xiLastReal));
    } else {
        c.psActive = mask || want.stick;
        c.psButtons = mask;
        c.psStick = want.stick;
        c.psLX = (uint8_t)std::clamp(128 + sin(a) * 127, 0.0, 255.0);
        c.psLY = (uint8_t)std::clamp(128 - cos(a) * 127, 0.0, 255.0);
        WriteData(g.ctrl + offsetof(PadCtrl, psActive), &c.psActive, offsetof(PadCtrl, psLastReal) - offsetof(PadCtrl, psActive));
    }
    g_applied = want;
}

void AutoRelease() {
    // 在上次实际使用的设备上松开
    InputDevice keep = g_dev;
    g_dev = g_appliedDev;
    Apply(Hold{});
    g_dev = keep;
    if (g.ok && g.ctrl) {
        PadCtrl zero{};
        WriteData(g.ctrl, &zero, offsetof(PadCtrl, xiLastReal));
        WriteData(g.ctrl + offsetof(PadCtrl, psActive), &zero.psActive, offsetof(PadCtrl, psLastReal) - offsetof(PadCtrl, psActive));
    }
    g_applied = Hold{};
}

// ---------------------------------------------------------------- 调度

struct Fired {
    uint64_t inst;
    int step;
    uint64_t at;
    uint64_t proj; // 飞行道具：连发的每一发各按一次
};
static std::deque<Fired> g_fired;

// 同一个音符每帧都会重新算出来，按过的不再按。蓝紫窗口的 atTsc 是步骤事件时间，同一窗口恒定（容差 0.6 秒）；
// 命中音符的是预测时刻，顿帧会让它后移零点几秒，容差 0.45 秒（同一招的同一击不会这么快再来）
static bool AlreadyFired(const Note& n) {
    bool span = n.kind == NoteKind::Blink || n.kind == NoteKind::Repulse;
    uint64_t tol = SecToTsc(span ? 0.6 : 0.45);
    for (auto& f : g_fired)
        if (f.inst == n.inst && f.step == n.step && !(f.proj && n.proj && f.proj != n.proj) &&
            (f.at > n.atTsc ? f.at - n.atTsc : n.atTsc - f.at) < tol)
            return true;
    return false;
}

// 正在执行的动作：到点按下按钮，按住一小会儿后全部松开。
// 蓝紫光：等窗口打开、且敌人进入触发距离（效果只对 4.5m 等范围内、敌人正面的目标生效，冲刺类招式前半段够不着），
// 再推住方向 0.12 秒（指令要求方向保持 0.1 秒）后按闪避；快到窗口末尾还没进距离就直接按。
struct Plan {
    bool active = false;
    Btn btn = Btn::Guard;
    Action action = Action::Parry;
    uint64_t pressAt = 0, releaseAt = 0; // pressAt=0：还在等条件
    bool stick = false, back = false;    // 摇杆朝向敌人 / 背离敌人
    uint64_t winStart = 0, deadline = 0, stickFrom = 0;
    float range = 0;
    Note note{};       // 对应的音符（没被游戏接受时重按用）
    bool fromNote = false; // 应对敌人攻击的格挡/闪避（连打、QTE、惩戒不算：不参与输入延迟统计和补按）
    bool retried = false;
};
// 按下后游戏没反应（伊芙没切步骤）时补按一次：记下最近一次格挡/闪避的按下
static Plan g_lastPress;
static Plan g_plan;
static uint64_t g_nextMash; // 连打的下一次
static uint64_t g_nextExec; // 惩戒按键冷却
// 惩戒时机：Groggy 标记在敌人刚被打崩时就置位，但要等它倒地动画走到一半（游戏里才真正进入 ActorState_Groggy）才能惩戒，
// 太早按 Y 会变成重攻击。实测渡鸦约 1~1.2 秒；按了却变成重攻击就把等待时间往后推 0.2 秒（本次运行记住）
static double g_execDelay = 1.2;
static uint64_t g_groggySince, g_execPressTsc;
static bool g_wasGroggy;

// 相机坐标系下“朝向敌人”的角度（度，0=相机正前，90=右）
static bool AngleToEnemy(double& deg) {
    float me[3], en[3], cl[3], cr[3], fov;
    if (!ActorLocation(g_live.player, me) || !ActorLocation(TargetEnemy(), en) || !CameraPov(cl, cr, fov)) return false;
    double yaw = atan2(en[1] - me[1], en[0] - me[0]) / kDegToRad;
    deg = yaw - cr[1];
    return true;
}

// 按住时长：游戏按“游戏时间”判断按键（闪避要按住 0.02 秒、蓝紫方向要保持 0.1 秒）。
// 伊芙被放慢时（Boss 爆发招的慢动作等）真实按住时间要相应拉长，否则太短被忽略
static uint64_t HoldTime(double sec) { return SecToTsc(sec / std::clamp(EveTimeScale(), 0.1f, 1.f)); }

// 立刻按一下某个按钮（按住 hold 秒）
static void PressNow(Btn b, uint64_t now, double hold) {
    g_plan = Plan{};
    g_plan.active = true;
    g_plan.btn = b;
    g_plan.pressAt = now;
    g_plan.releaseAt = now + HoldTime(hold);
}

// 完美闪避成功后伊芙有一段专属动作（JustEvade Cast1+Cast2，游戏时间 0.6 秒，外加慢动作，实测约 0.88 秒），全程无敌、
// 也不能再闪避。这期间按闪避会被游戏缓存到动作结束才打出来，变成一个时机错掉的普通闪避，还把下一下攻击的闪避占掉了
// （红莲 BackDashSpaceCut 第 5、6 下间隔 0.6 秒，就是这样连着挨打）。落在这段里的攻击不按。
// 只是暂缓、不记成已处理：远程招的预测会变（红莲 PhaseChange2_AttackRange_Hit3 预测在无敌内，实际晚了 0.5 秒才到），
// 无敌结束后若还没到，照常按
static const double kJustEvadeInvuln = 0.85;

// 敌人的攻击：到点按格挡 / 闪避 / 跳跃；蓝紫光排一个“推方向 + 闪避”
static bool ScheduleNotes(uint64_t now) {
    const double lat = InputLatency();
    // 完美窗口正中 = 结算前 window/2；再提前一个输入延迟
    // 连招里紧接在弹反后的一击常比预测晚（弹反的顿帧），整体略偏窗口后段，早了的余量更大
    const double aim = g_window / 2 + lat - 0.025 - g_cfg.autoAimMs / 1000.0;
    for (const Note& n : g_notes) {
        if (AlreadyFired(n)) continue;
        // 顿帧中（流速 < 0.8）敌人几乎不动，剩余的游戏时间对应的真实时间更长：按流速换算后再比较
        double tReal = n.rate < 0.8f ? n.t / std::max(0.05f, n.rate) : n.t;
        bool hit = n.kind == NoteKind::Parry || n.kind == NoteKind::Evade || n.kind == NoteKind::Danger || n.kind == NoteKind::Jump;
        uint64_t je = LastJustEvadeTsc();
        if (hit && n.t >= 0 && je && now < je + SecToTsc(kJustEvadeInvuln) && n.atTsc < je + SecToTsc(kJustEvadeInvuln)) {
            static std::pair<uint64_t, int> logged;
            if (logged != std::make_pair(n.inst, n.step)) {
                logged = {n.inst, n.step};
                TimingNote("skip-invuln", n, EveTimeScale());
            }
            continue;
        }
        if ((n.kind == NoteKind::Parry || n.kind == NoteKind::Evade || n.kind == NoteKind::Danger) && n.t >= 0 && tReal <= aim) {
            // 红色（不能弹反也不能完美闪避）：普通闪避，靠闪避的无敌帧躲开
            PressNow(n.kind == NoteKind::Parry ? Btn::Guard : Btn::Evade, now, 0.08);
            g_plan.action = n.kind == NoteKind::Parry ? Action::Parry : Action::Evade;
        } else if (n.kind == NoteKind::Jump && n.t >= 0 && tReal <= 0.25 + lat) {
            // 环只有 40cm 高：环到时伊芙要已经跳高。早 0.2 秒按实测来不及（刚起跳就被打到），提前 0.25 秒 + 输入延迟，
            // 跳跃滞空远长于此，环到时正在空中
            PressNow(Btn::Jump, now, 0.1);
        } else if ((n.kind == NoteKind::Blink || n.kind == NoteKind::Repulse) && g_cfg.autoChance && n.t <= 0.02 &&
                   n.t + n.len > 0.05) {
            Plan p;
            p.active = true;
            p.btn = Btn::Evade;
            p.action = n.kind == NoteKind::Blink ? Action::Blink : Action::Repulse;
            p.stick = true;
            p.back = n.kind == NoteKind::Repulse;
            p.range = n.range;
            p.winStart = now + (uint64_t)((int64_t)(n.t * g_tscPerSec));
            p.deadline = now + SecToTsc(std::max(0.0, n.t + n.len - 0.12 - lat - 0.05));
            // 紫光多是冲刺招：敌人要冲到身前、面朝伊芙时效果才对她生效。实测成功都在窗口最后 0.2~0.4 秒，按窗口末段出手
            if (n.kind == NoteKind::Repulse) p.winStart = std::max(p.winStart, p.deadline - SecToTsc(0.08));
            g_plan = p;
        } else {
            continue;
        }
        g_plan.note = n;
        g_plan.fromNote = true;
        g_fired.push_back({n.inst, n.step, n.atTsc, n.proj});
        TimingNote(n.kind == NoteKind::Parry ? "guard" : n.kind == NoteKind::Jump ? "jump" : "evade", n, EveTimeScale());
        return true;
    }
    return false;
}

// 惩戒：敌人倒地可惩戒、等过倒地动画、且在 3m 内（游戏的条件还要求在正前方，伊芙出招时会自己转向）就按重攻击。
// 开始惩戒后 Groggy 立刻清掉，所以不会重复。没进距离就等着
// 每帧跟踪倒地时刻（不论这一帧有没有轮到惩戒，否则等待时间会从很早以前算起）
static void TrackGroggy(uint64_t now, bool on) {
    bool groggy = on && g_cfg.autoQte && EnemyGroggy();
    if (groggy && !g_wasGroggy) g_groggySince = now;
    g_wasGroggy = groggy;
}

static bool ScheduleFinisher(uint64_t now) {
    bool groggy = g_wasGroggy;
    // 上一次按 Y 的结果：变成了重攻击 = 太早
    if (g_execPressTsc && LastEveStepTsc() > g_execPressTsc) {
        if (LastEveStepName().find("StrongAttack") != std::string::npos) {
            g_execDelay = std::min(3.0, g_execDelay + 0.2);
            g_nextExec = now + SecToTsc(0.9); // 等重攻击打完，免得连成二段重攻击
            Log(TR("  [自动] 惩戒按早了（变成重攻击），之后等 %.1f 秒再按\n", "  [auto] Finisher too early (heavy attack); waiting %.1fs from now on\n"),
                g_execDelay);
        }
        g_execPressTsc = 0;
    }
    if (!groggy || now < g_nextExec || now - g_groggySince < SecToTsc(g_execDelay)) return false;
    float d = TargetDistance();
    if (d <= 0 || d > 2.8f) return false;
    PressNow(Btn::Strong, now, 0.06);
    g_nextExec = now + SecToTsc(0.4);
    g_execPressTsc = now;
    Log(TR("  [自动] 惩戒（距离 %.1fm）\n", "  [auto] Finisher (distance %.1fm)\n"), d);
    return true;
}

// 连打：拼刀/被抓挣脱（所有敌人通用，游戏里都是轻攻击），或过场 QTE（按控件要求的动作；单次按键多按几下也无妨）
static void ScheduleMash(uint64_t now) {
    Btn mash = Btn::Count;
    int qte = g_cfg.autoQte ? CutsceneQteAction() : -1;
    if (qte >= 0) {
        auto it = g_actionByName.find((uint32_t)qte);
        mash = it != g_actionByName.end() ? it->second : Btn::Light;
    } else if (MashActive()) {
        mash = Btn::Light;
    }
    if (mash == Btn::Count || now < g_nextMash) return;
    PressNow(mash, now, 0.035);
    g_nextMash = now + HoldTime(0.07);
}

static void Schedule(uint64_t now) {
    if (ScheduleNotes(now) || ScheduleFinisher(now)) return;
    ScheduleMash(now);
}

void AutoTick() {
    static bool wasOn = false;
    // 玩家按着 Ctrl/Alt（比如刚按完 Ctrl+Alt+A）时不发键，免得和快捷键拼成组合键
    bool mods = (GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000;
    bool fg = GameForeground();
    bool on = g_cfg.autoParry && g.ok && StepsLoaded() && fg && !mods;
    static bool lastFg = true;
    if (g_cfg.autoParry && g.ok && fg != lastFg)
        Log(fg ? TR("[SBParry] 游戏回到前台，自动继续\n", "[SBParry] Game focused, auto resumed\n")
               : TR("[SBParry] 游戏不在前台，自动暂停\n", "[SBParry] Game not focused, auto paused\n"));
    lastFg = fg;
    uint64_t now = __rdtsc();
    TrackGroggy(now, on);
    if (!on) {
        if (wasOn) { AutoRelease(); g_plan.active = false; }
        wasOn = false;
        return;
    }
    wasOn = true;
    LoadKeys();
    while (!g_fired.empty() && now > g_fired.front().at + SecToTsc(4)) g_fired.pop_front();

    // 格挡/闪避按下 0.14 秒后（输入延迟约 0.07 秒）伊芙还没有任何步骤切换：输入没被游戏接受，离命中还有时间就补按一次
    if (!g_plan.active && g_lastPress.active && !g_lastPress.retried && now - g_lastPress.pressAt > SecToTsc(0.14)) {
        g_lastPress.active = false;
        double left = TscToSec((int64_t)(g_lastPress.note.atTsc - now));
        if (LastEveStepTsc() < g_lastPress.pressAt && left > 0.03) {
            Log(TR("  [自动] 按键没被接受，补按一次（%s，离命中 %dms）\n", "  [auto] press not accepted, retrying (%s, %dms before hit)\n"),
                AutoDeviceName(), Ms(left));
            Plan p = g_lastPress;
            p.active = true;
            p.retried = true;
            p.pressAt = now;
            p.releaseAt = now + HoldTime(0.08);
            g_plan = p;
        }
    }
    if (!g_plan.active) {
        InputDevice d = PickDevice();
        if (d != g_dev) {
            AutoRelease();
            g_dev = d;
            Log(TR("[SBParry] 自动操作输入设备：%s\n", "[SBParry] Auto input device: %s\n"), AutoDeviceName());
        }
        Schedule(now);
        if (!g_plan.active) return;
    }
    Plan& p = g_plan;
    if (!p.pressAt) {
        float dist = TargetDistance();
        bool inRange = dist < 0 || p.range <= 0 || dist <= p.range - 0.3f;
        if ((now >= p.winStart && inRange) || now >= p.deadline) {
            Log(TR("  [自动] %s 出手：距离 %.1fm（触发距离 %.1fm）%s\n", "  [auto] %s: distance %.1fm (range %.1fm)%s\n"),
                p.action == Action::Blink ? TR("前闪", "Blink") : TR("后闪", "Repulse"), dist, p.range,
                now >= p.deadline ? TR("，到窗口末尾", ", window ending") : L"");
            p.stickFrom = now;
            p.pressAt = now + HoldTime(0.12);
            p.releaseAt = p.pressAt + HoldTime(0.08);
        }
    }

    Hold want;
    double a = 0;
    if (p.stick && p.pressAt && now >= p.stickFrom && AngleToEnemy(a)) {
        want.stick = true;
        want.angle = p.back ? a + 180 : a;
    }
    if (p.pressAt && now >= p.pressAt) {
        if (!g_applied.btn[(int)p.btn] && p.fromNote && (p.btn == Btn::Guard || p.btn == Btn::Evade)) {
            MarkAutomatic(p.action, now);
            if (!p.stick) { g_lastPress = p; g_lastPress.pressAt = now; g_lastPress.active = !p.retried; }
        }
        want.btn[(int)p.btn] = true;
    }
    if (p.pressAt && now >= p.releaseAt) {
        want = Hold{};
        p.active = false;
    }
    Apply(want);
}
