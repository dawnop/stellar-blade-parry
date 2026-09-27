// 叠加层离线预览：不连游戏，把若干 HudFrame 场景渲染成 PNG（深色 / 亮色 / 渐变三种背景）
// 构建见 build_preview.bat；输出到当前目录 out/
#include "overlay.h"
#include "config.h"
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <chrono>
using namespace Gdiplus;

Lang g_lang = Lang::Zh;
Config g_cfg;
double g_tscPerSec = 1e9;
void Log(const wchar_t*, ...) {}

// overlay.cpp 的测试钩子
void OverlayRenderBarToBitmap(const HudFrame& f, Bitmap& bmp);
SIZE OverlayPanelSize();
void OverlayRenderPanelToBitmap(const HudFrame& f, Bitmap& bmp);
SIZE OverlayToastSize(const HudFrame& f);
void OverlayRenderToastToBitmap(const HudFrame& f, Bitmap& bmp);

static CLSID PngClsid() {
    UINT n = 0, sz = 0;
    GetImageEncodersSize(&n, &sz);
    std::vector<BYTE> buf(sz);
    auto* enc = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, sz, enc);
    for (UINT i = 0; i < n; i++)
        if (!wcscmp(enc[i].MimeType, L"image/png")) return enc[i].Clsid;
    return CLSID{};
}

// 三栏背景：深色、亮色、仿游戏的亮天空渐变
static void Backgrounds(Graphics& g, int cw, int ch) {
    SolidBrush dark(Color(255, 0x15, 0x17, 0x1b)), bright(Color(255, 0xd8, 0xd2, 0xc4));
    g.FillRectangle(&dark, 0, 0, cw, ch);
    g.FillRectangle(&bright, cw, 0, cw, ch);
    LinearGradientBrush sky(Point(2 * cw, 0), Point(3 * cw, ch), Color(255, 250, 244, 225), Color(255, 70, 110, 150));
    g.FillRectangle(&sky, 2 * cw, 0, cw, ch);
    SolidBrush sun(Color(255, 255, 255, 250));
    g.FillEllipse(&sun, 2 * cw + cw / 3, ch / 4, cw / 3, ch / 2);
}

static void Save(Bitmap& ov, const wchar_t* name) {
    int pad = 24, cw = ov.GetWidth() + 2 * pad, ch = ov.GetHeight() + 2 * pad;
    Bitmap out(3 * cw, ch, PixelFormat32bppARGB);
    Graphics g(&out);
    Backgrounds(g, cw, ch);
    for (int i = 0; i < 3; i++) g.DrawImage(&ov, i * cw + pad, pad);
    CreateDirectoryW(L"out", nullptr);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"out\\%ls.png", name);
    CLSID png = PngClsid();
    out.Save(path, &png);
    wprintf(L"%ls\n", path);
    // 放大 3 倍（最近邻）的深色 + 亮色两栏，上下排，便于看细节
    const int z = 3;
    Bitmap zb(cw * z, 2 * ch * z, PixelFormat32bppARGB);
    Graphics zg(&zb);
    zg.SetInterpolationMode(InterpolationModeNearestNeighbor);
    zg.SetPixelOffsetMode(PixelOffsetModeHalf);
    for (int i = 0; i < 2; i++) zg.DrawImage(&out, Rect(0, i * ch * z, cw * z, ch * z), i * cw, 0, cw, ch, UnitPixel);
    swprintf(path, MAX_PATH, L"out\\zoom\\%ls.png", name);
    CreateDirectoryW(L"out\\zoom", nullptr);
    zb.Save(path, &png);
}

static Note N(NoteKind k, double t, double len = 0, float range = 0) { return Note{k, t, len, 0, 0, 0, range, 1.f, t, 0}; }
static Result R(Action a, Verdict v, double lead, double off, int ageMs, bool autom = false) {
    return Result{a, v, lead, 0.217, off, GetTickCount64() - ageMs, autom};
}

struct Scene {
    const wchar_t* name;
    Lang lang;
    int scale;
    std::vector<Note> notes;
    std::deque<Result> results;
    bool autoOn = false;
    const wchar_t* status = nullptr;
    float dist = -1;
    double now = 0;
};

static HudFrame Frame(const Scene& sc) {
    HudFrame f{};
    f.connected = !sc.status;
    f.status = sc.status;
    f.window = 13 / 60.0 - 1 / 60.0;
    f.horizon = 1.0;
    f.notes = &sc.notes;
    f.results = &sc.results;
    f.autoOn = sc.autoOn;
    f.targetDistance = sc.dist;
    f.now = sc.now;
    return f;
}

static void Bar(const Scene& sc) {
    g_lang = sc.lang;
    g_cfg.uiScale = sc.scale;
    HudFrame f = Frame(sc);
    SIZE sz = OverlayBarSize();
    Bitmap bmp(sz.cx, sz.cy, PixelFormat32bppPARGB);
    OverlayRenderBarToBitmap(f, bmp);
    Save(bmp, sc.name);
}

int main() {
    ULONG_PTR tok;
    GdiplusStartupInput in;
    GdiplusStartup(&tok, &in, nullptr); // 预览自己的 PNG 编码用；overlay 另有一份引用计数
    OverlayInit(GetModuleHandleW(nullptr), DefWindowProcW);
    using NK = NoteKind;
    std::vector<Scene> scenes = {
        {L"01_idle", Lang::Zh, 100, {}, {}},
        {L"02_waiting", Lang::Zh, 100, {}, {}, false, L"等待游戏…"},
        {L"03_parry_approach", Lang::Zh, 100, {N(NK::Parry, 0.42), N(NK::Evade, 0.71), N(NK::Danger, 0.93)}, {}},
        {L"04_parry_in_zone", Lang::Zh, 100, {N(NK::Parry, 0.09), N(NK::Parry, 0.55)}, {}},
        {L"05_blink_active_far", Lang::Zh, 100, {N(NK::Blink, -0.12, 0.45, 8), N(NK::Parry, 0.62)}, {}, false, nullptr, 12.4f, 0.04},
        {L"06_repulse_span", Lang::Zh, 100, {N(NK::Parry, 0.18), N(NK::Repulse, 0.36, 0.4, 6)}, {}},
        {L"07_perfect_pop", Lang::Zh, 100, {N(NK::Parry, 0.66)}, {R(Action::Parry, Verdict::Perfect, 0.05, 0, 300)}},
        {L"08_early_pop", Lang::Zh, 100, {}, {R(Action::Parry, Verdict::Early, 0.3, 0.083, 400)}},
        {L"09_auto_perfect", Lang::Zh, 100, {N(NK::Evade, 0.3)}, {R(Action::Evade, Verdict::Perfect, 0.1, 0, 250, true)}, true},
        {L"10_en_blink_far", Lang::En, 100, {N(NK::Blink, 0.05, 0.4, 8), N(NK::Danger, 0.5)}, {R(Action::Parry, Verdict::Late, 0, 0.05, 500)}, false, nullptr, 11.2f},
        {L"11_en_perfect_in_zone", Lang::En, 100, {N(NK::Parry, 0.1), N(NK::Evade, 0.8)}, {R(Action::Parry, Verdict::Perfect, 0.033, 0, 350)}, true},
        {L"12_en_repulse_active_missed", Lang::En, 100, {N(NK::Repulse, -0.2, 0.5, 6)}, {R(Action::Repulse, Verdict::Missed, 0, 0, 200)}, false, nullptr, 4, 0.08},
        {L"13_zh_150_in_zone", Lang::Zh, 150, {N(NK::Parry, 0.1), N(NK::Blink, 0.45, 0.3, 8)}, {R(Action::Blink, Verdict::Perfect, 0, 0, 300)}, true},
        {L"14_popin_early_frame", Lang::En, 100, {}, {R(Action::Parry, Verdict::Perfect, 0.05, 0, 40)}},
        {L"15_jump_ring", Lang::Zh, 100, {N(NK::Jump, 0.2), N(NK::Parry, 0.7)}, {}, true},
    };
    for (auto& sc : scenes) Bar(sc);

    // 面板 + 提示
    for (Lang l : {Lang::Zh, Lang::En}) {
        g_lang = l;
        g_cfg.uiScale = 100;
        Scene sc{L"", l, 100, {}, {}};
        sc.autoOn = true;
        for (int i = 0; i < 17; i++)
            sc.results.push_back(R(i % 3 == 1 ? Action::Evade : Action::Parry, i % 5 == 1 ? Verdict::Early : i % 7 == 2 ? Verdict::Late : Verdict::Perfect, 0.05, 0.05, 5000, i == 0));
        HudFrame f = Frame(sc);
        SIZE ps = OverlayPanelSize();
        Bitmap pb(ps.cx, ps.cy, PixelFormat32bppPARGB);
        OverlayRenderPanelToBitmap(f, pb);
        Save(pb, l == Lang::Zh ? L"20_panel_zh" : L"21_panel_en");
        f.connected = false;
        f.results = nullptr;
        f.autoOn = false;
        OverlayRenderPanelToBitmap(f, pb);
        Save(pb, l == Lang::Zh ? L"22_panel_zh_waiting" : L"23_panel_en_waiting");
        f.toast = l == Lang::Zh ? L"自动弹反：开 (Ctrl+Alt+A)" : L"AUTO PARRY: ON (Ctrl+Alt+A)";
        SIZE ts = OverlayToastSize(f);
        Bitmap tb(ts.cx, ts.cy, PixelFormat32bppPARGB);
        OverlayRenderToastToBitmap(f, tb);
        Save(tb, l == Lang::Zh ? L"30_toast_zh" : L"31_toast_en");
    }

    // 耗时：判定条每帧渲染，音符每帧移动（倒计时数字每帧变，文字缓存会不断换新）
    auto bench = [](const wchar_t* label, Scene sc) {
        g_lang = Lang::Zh;
        g_cfg.uiScale = 100;
        SIZE sz = OverlayBarSize();
        Bitmap bmp(sz.cx, sz.cy, PixelFormat32bppPARGB);
        const int n = 600;
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++) {
            for (auto& note : sc.notes) note.tShow = note.t -= 0.9 / n;
            sc.now = i / 144.0;
            for (auto& r : sc.results) r.shownAt = GetTickCount64() - 300;
            HudFrame f = Frame(sc);
            OverlayRenderBarToBitmap(f, bmp);
        }
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
        wprintf(L"bench %-10ls %6.0f us/frame\n", label, us);
    };
    bench(L"empty", Scene{L"", Lang::Zh, 100, {}, {}});
    bench(L"notes", Scene{L"", Lang::Zh, 100, {N(NK::Parry, 0.3), N(NK::Evade, 0.7), N(NK::Danger, 0.9)}, {}});
    bench(L"spans", Scene{L"", Lang::Zh, 100, {N(NK::Blink, -0.1, 0.4, 8), N(NK::Repulse, 0.5, 0.4, 8)}, {}});
    bench(L"pop", Scene{L"", Lang::Zh, 100, {}, {R(Action::Parry, Verdict::Perfect, 0.05, 0, 300)}});
    bench(L"all", Scene{L"", Lang::Zh, 100, {N(NK::Parry, 0.3), N(NK::Blink, -0.1, 0.4, 8), N(NK::Evade, 0.7)}, {R(Action::Parry, Verdict::Perfect, 0.05, 0, 300)}, true});
    OverlayShutdown();
    GdiplusShutdown(tok);
    return 0;
}
