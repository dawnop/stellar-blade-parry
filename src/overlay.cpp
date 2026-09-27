// 叠加层：GDI+ 抗锯齿绘制到 32 位预乘 ARGB DIB，UpdateLayeredWindow(ULW_ALPHA) 逐像素 alpha 呈现
// 风格仿剑星 UI：细亮线 + 半透明近黑底 + 角标，文字用柔和暗影保证亮场景可读
#include "overlay.h"
#include "config.h"
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <unordered_map>
#pragma comment(lib, "gdiplus.lib")
using namespace Gdiplus;

// ---------------------------------------------------------------- 颜色

static const Color kInk(255, 237, 237, 237), kDim(255, 150, 156, 166), kShade(255, 4, 6, 9), kPlate(255, 10, 12, 16);
static const Color kParry(255, 127, 216, 255), kEvade(255, 255, 194, 74), kDanger(255, 255, 90, 90), kJump(255, 120, 235, 200);
static const Color kBlink(255, 61, 123, 255), kRepulse(255, 176, 101, 255);
static const Color kZone(255, 75, 227, 138), kZoneLit(255, 120, 255, 168);
static const Color kGood(255, 92, 255, 143), kGoodText(255, 236, 255, 242);
static const Color kEarly(255, 255, 181, 71), kLate(255, 140, 203, 255), kMiss(255, 255, 96, 96);
static const Color kNone(0, 0, 0, 0);

static Color Al(const Color& c, float a) {
    return Color((BYTE)std::clamp(c.GetA() * a, 0.f, 255.f), c.GetR(), c.GetG(), c.GetB());
}
static Color Mix(const Color& a, const Color& b, float t) {
    auto m = [t](BYTE x, BYTE y) { return (BYTE)std::lround(x + (y - x) * t); };
    return Color(m(a.GetA(), b.GetA()), m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
}
static bool IsSpan(NoteKind k) { return k == NoteKind::Blink || k == NoteKind::Repulse; }
static Color KindColor(NoteKind k) {
    switch (k) {
    case NoteKind::Parry: return kParry;
    case NoteKind::Evade: return kEvade;
    case NoteKind::Danger: return kDanger;
    case NoteKind::Jump: return kJump;
    case NoteKind::Blink: return kBlink;
    default: return kRepulse;
    }
}

// ---------------------------------------------------------------- 全局 GDI+ 对象（复用，避免每帧分配）

static ULONG_PTR g_gdip;
static Pen* g_pen;
static SolidBrush* g_brush;
static GraphicsPath* g_tmp;
static StringFormat* g_fmt;
static Bitmap* g_measureBmp;
static Graphics* g_measure;
static FontFamily *g_latB, *g_cjk; // 拉丁（半粗）/ 中文
static int g_latBStyle = FontStyleRegular, g_cjkStyle = FontStyleBold;
static float g_scale = 0;

static void SetPen(const Color& c, float w) { g_pen->SetColor(c); g_pen->SetWidth(w); }
static void SetBrush(const Color& c) { g_brush->SetColor(c); }

static void Line(Graphics& g, float x0, float y0, float x1, float y1, const Color& c, float w) {
    SetPen(c, w);
    g.DrawLine(g_pen, x0, y0, x1, y1);
}
static void Box(Graphics& g, float x, float y, float w, float h, const Color& c) {
    SetBrush(c);
    g.FillRectangle(g_brush, x, y, w, h);
}
static void Disc(Graphics& g, float cx, float cy, float r, const Color& c) {
    SetBrush(c);
    g.FillEllipse(g_brush, cx - r, cy - r, 2 * r, 2 * r);
}
static void RoundPath(GraphicsPath& p, float x, float y, float w, float h, float r) {
    p.Reset();
    r = std::min(r, std::min(w, h) / 2);
    float d = 2 * r;
    if (d < 0.5f) { p.AddRectangle(RectF(x, y, w, h)); return; }
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}
// 四角的 L 形角标（代替整框）
static void Brackets(Graphics& g, float x, float y, float w, float h, float len, const Color& c, float lw) {
    SetPen(c, lw);
    float o = lw / 2;
    x += o, y += o, w -= lw, h -= lw;
    PointF tl[] = {{x, y + len}, {x, y}, {x + len, y}};
    PointF tr[] = {{x + w - len, y}, {x + w, y}, {x + w, y + len}};
    PointF br[] = {{x + w, y + h - len}, {x + w, y + h}, {x + w - len, y + h}};
    PointF bl[] = {{x + len, y + h}, {x, y + h}, {x, y + h - len}};
    g.DrawLines(g_pen, tl, 3);
    g.DrawLines(g_pen, tr, 3);
    g.DrawLines(g_pen, br, 3);
    g.DrawLines(g_pen, bl, 3);
}
// 近黑半透明底板 + 细边 + 角标
static void Plate(Graphics& g, float x, float y, float w, float h, float s, float alpha = 1) {
    Box(g, x, y, w, h, Al(Color(165, 10, 12, 16), alpha));
    SetPen(Al(Color(46, 237, 237, 237), alpha), 1);
    g.DrawRectangle(g_pen, x + 0.5f, y + 0.5f, w - 1, h - 1);
    Brackets(g, x, y, w, h, 9 * s, Al(Color(215, 237, 237, 237), alpha), std::max(1.f, 1.5f * s));
}

// ---------------------------------------------------------------- 文字：预渲染成精灵缓存
// 逐字排版（拉丁字母加字距、中文换字体），DrawString 抗锯齿出字形遮罩，CPU 模糊出柔和暗影/辉光；
// 每帧只 DrawImage，画宽笔描边太慢（一行字 >1ms）

enum Role { kPop, kInfo, kSmall, kToast, kRoleCount };
struct RoleDef { float size, track, cjkSize, cjkTrack; };
static const RoleDef kRoles[kRoleCount] = {
    {25, 2.6f, 23, 2.0f},    // 判定弹字
    {15, 0.8f, 14, 0.6f},    // 信息行
    {10.5f, 1.8f, 11, 0.6f}, // 小标签
    {15, 1.4f, 15, 0.8f},    // 提示
};
struct RoleFont {
    Font *lat = nullptr, *cjk = nullptr;
    float px = 0, ascL = 0, ascC = 0, base = 0, lineH = 0, track = 0, ctrack = 0;
};
static RoleFont g_rf[kRoleCount];

struct Sprite { Bitmap* bmp; float w; int pad; };
static std::unordered_map<std::wstring, Sprite> g_sprites;
static std::unordered_map<uint32_t, float> g_adv;
static ImageAttributes* g_ia;
static std::wstring g_key; // 复用的查找键

static bool IsCjk(wchar_t c) { return c >= 0x2E80 || (c >= 0x2000 && c <= 0x206F && c != 0x2019); }

static void ClearTextCache() {
    for (auto& [k, sp] : g_sprites) delete sp.bmp;
    g_sprites.clear();
    g_adv.clear();
}

static void BuildFonts(float s) {
    ClearTextCache();
    for (int i = 0; i < kRoleCount; i++) {
        RoleFont& rf = g_rf[i];
        delete rf.lat;
        delete rf.cjk;
        const RoleDef& d = kRoles[i];
        float cpx = d.cjkSize * s;
        rf.px = d.size * s;
        rf.track = d.track * s;
        rf.ctrack = d.cjkTrack * s;
        rf.lat = new Font(g_latB, rf.px, g_latBStyle, UnitPixel);
        rf.cjk = new Font(g_cjk, cpx, g_cjkStyle, UnitPixel);
        auto asc = [](FontFamily* f, int st, float px) { return px * f->GetCellAscent(st) / f->GetEmHeight(st); };
        auto dsc = [](FontFamily* f, int st, float px) { return px * f->GetCellDescent(st) / f->GetEmHeight(st); };
        rf.ascL = asc(g_latB, g_latBStyle, rf.px);
        rf.ascC = asc(g_cjk, g_cjkStyle, cpx);
        rf.base = std::max(rf.ascL, rf.ascC);
        rf.lineH = rf.base + std::max(dsc(g_latB, g_latBStyle, rf.px), dsc(g_cjk, g_cjkStyle, cpx));
    }
    g_scale = s;
}

static float Advance(int role, bool cjk, wchar_t c) {
    uint32_t key = (uint32_t)role << 17 | (uint32_t)cjk << 16 | c;
    auto it = g_adv.find(key);
    if (it != g_adv.end()) return it->second;
    RectF box;
    g_measure->MeasureString(&c, 1, cjk ? g_rf[role].cjk : g_rf[role].lat, PointF(0, 0), g_fmt, &box);
    return g_adv[key] = box.Width;
}

// 逐字排版；draw 非空时把字画出来
static float Layout(int role, const wchar_t* s, Graphics* draw = nullptr, Brush* b = nullptr, float ox = 0, float oy = 0) {
    const RoleFont& rf = g_rf[role];
    float x = 0, lastTrack = 0;
    for (const wchar_t* c = s; *c; c++) {
        bool cjk = IsCjk(*c);
        if (draw && *c != L' ')
            draw->DrawString(c, 1, cjk ? rf.cjk : rf.lat, PointF(ox + x, oy + rf.base - (cjk ? rf.ascC : rf.ascL)), g_fmt, b);
        lastTrack = cjk ? rf.ctrack : rf.track;
        x += Advance(role, cjk, *c) + lastTrack;
    }
    return std::max(0.f, x - lastTrack);
}
static float TextWidth(int role, const wchar_t* s) { return Layout(role, s); }

// 两次盒式模糊 ≈ 高斯
static void Blur(std::vector<float>& a, std::vector<float>& tmp, int w, int h, int r) {
    if (r < 1) return;
    tmp.resize(a.size());
    float inv = 1.f / (2 * r + 1);
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) {
            const float* src = &a[(size_t)y * w];
            float* dst = &tmp[(size_t)y * w];
            float sum = 0;
            for (int x = -r; x <= r; x++) sum += (x >= 0 && x < w) ? src[x] : 0;
            for (int x = 0; x < w; x++) {
                dst[x] = sum * inv;
                int xo = x - r, xi = x + r + 1;
                if (xo >= 0) sum -= src[xo];
                if (xi < w) sum += src[xi];
            }
        }
        for (int x = 0; x < w; x++) {
            float sum = 0;
            for (int y = -r; y <= r; y++) sum += (y >= 0 && y < h) ? tmp[(size_t)y * w + x] : 0;
            for (int y = 0; y < h; y++) {
                a[(size_t)y * w + x] = sum * inv;
                int yo = y - r, yi = y + r + 1;
                if (yo >= 0) sum -= tmp[(size_t)yo * w + x];
                if (yi < h) sum += tmp[(size_t)yi * w + x];
            }
        }
    }
}

static Sprite BuildSprite(int role, const wchar_t* s, const Color& fill, const Color& glow) {
    const RoleFont& rf = g_rf[role];
    float w = TextWidth(role, s);
    int pad = (int)std::ceil(7 * g_scale) + 1;
    int bw = (int)std::ceil(w) + 2 * pad + 1, bh = (int)std::ceil(rf.lineH) + 2 * pad;
    Bitmap mask(bw, bh, PixelFormat32bppPARGB);
    {
        Graphics g(&mask);
        g.Clear(Color(0, 0, 0, 0));
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        SolidBrush white(Color(255, 255, 255, 255));
        Layout(role, s, &g, &white, (float)pad, (float)pad);
    }
    static std::vector<float> A, T, wide, tight;
    size_t n = (size_t)bw * bh;
    A.resize(n);
    Rect rc(0, 0, bw, bh);
    BitmapData d;
    mask.LockBits(&rc, ImageLockModeRead, PixelFormat32bppPARGB, &d);
    for (int y = 0; y < bh; y++) {
        const BYTE* row = (const BYTE*)d.Scan0 + (size_t)y * d.Stride;
        for (int x = 0; x < bw; x++) A[(size_t)y * bw + x] = row[x * 4 + 3] / 255.f;
    }
    mask.UnlockBits(&d);
    wide = A, tight = A;
    Blur(wide, T, bw, bh, std::max(1, (int)std::lround(2.4f * g_scale)));
    Blur(tight, T, bw, bh, std::max(1, (int)std::lround(0.9f * g_scale)));

    auto* out = new Bitmap(bw, bh, PixelFormat32bppPARGB);
    out->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppPARGB, &d);
    float fa = fill.GetA() / 255.f, ga = glow.GetA() / 255.f;
    float fr = fill.GetR(), fg = fill.GetG(), fb = fill.GetB(), gr = glow.GetR(), gg = glow.GetG(), gb = glow.GetB();
    for (int y = 0; y < bh; y++) {
        BYTE* row = (BYTE*)d.Scan0 + (size_t)y * d.Stride;
        for (int x = 0; x < bw; x++) {
            size_t i = (size_t)y * bw + x;
            // 外圈宽而淡的暗影 → 辉光 → 贴字一圈柔和暗边（亮背景上的轮廓，不是硬描边）→ 字
            float wv = wide[i], tv = tight[i];
            float a = std::min(1.f, 1.7f * wv) * 0.45f, r = 4 * a, g = 6 * a, b = 9 * a;
            if (ga > 0) {
                float gl = std::min(1.f, 2.4f * wv) * 0.6f * ga;
                r = gr * gl + r * (1 - gl), g = gg * gl + g * (1 - gl), b = gb * gl + b * (1 - gl), a = gl + a * (1 - gl);
            }
            float sh = std::min(1.f, 2.4f * tv) * (ga > 0 ? 0.42f : 0.5f);
            r = 4 * sh + r * (1 - sh), g = 6 * sh + g * (1 - sh), b = 9 * sh + b * (1 - sh), a = sh + a * (1 - sh);
            float f = A[i] * fa;
            r = fr * f + r * (1 - f), g = fg * f + g * (1 - f), b = fb * f + b * (1 - f), a = f + a * (1 - f);
            BYTE* p = row + x * 4;
            p[0] = (BYTE)std::lround(std::min(b, 255.f));
            p[1] = (BYTE)std::lround(std::min(g, 255.f));
            p[2] = (BYTE)std::lround(std::min(r, 255.f));
            p[3] = (BYTE)std::lround(std::min(a, 1.f) * 255);
        }
    }
    out->UnlockBits(&d);
    return Sprite{out, w, pad};
}

static const Sprite& GetSprite(int role, const wchar_t* s, const Color& fill, const Color& glow) {
    ARGB f = fill.GetValue(), gl = glow.GetValue();
    g_key.clear();
    g_key += (wchar_t)('A' + role);
    g_key += (wchar_t)(f >> 16), g_key += (wchar_t)(f & 0xFFFF), g_key += (wchar_t)(gl >> 16), g_key += (wchar_t)(gl & 0xFFFF);
    g_key += s;
    auto it = g_sprites.find(g_key);
    if (it != g_sprites.end()) return it->second;
    if (g_sprites.size() > 160) {
        for (auto& [k, sp] : g_sprites) delete sp.bmp;
        g_sprites.clear();
    }
    return g_sprites[g_key] = BuildSprite(role, s, fill, glow);
}

static void Blit(Graphics& g, Bitmap* b, float x, float y, float alpha) {
    float w = (float)b->GetWidth(), h = (float)b->GetHeight();
    if (alpha >= 0.999f) {
        g.DrawImage(b, RectF(x, y, w, h), 0, 0, w, h, UnitPixel);
        return;
    }
    ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha, 0}, {0, 0, 0, 0, 1}}};
    g_ia->SetColorMatrix(&cm);
    g.DrawImage(b, RectF(x, y, w, h), 0, 0, w, h, UnitPixel, g_ia);
}

// (x, cy)：左端/中点/右端 与 视觉中线；在变换下（弹字缩放）snap=false 不取整
enum Align { kLeft, kCenter, kRight };
static float Text(Graphics& g, int role, const wchar_t* s, float x, float cy, Align al, const Color& fill,
                  const Color& glow = kNone, float alpha = 1, bool snap = true) {
    if (!s || !*s || alpha <= 0.01f) return 0;
    const Sprite& sp = GetSprite(role, s, fill, glow);
    const RoleFont& rf = g_rf[role];
    if (al == kCenter) x -= sp.w / 2;
    else if (al == kRight) x -= sp.w;
    float px = x - sp.pad, py = cy + 0.36f * rf.px - rf.base - sp.pad;
    if (snap) px = std::round(px), py = std::round(py);
    Blit(g, sp.bmp, px, py, alpha);
    return sp.w;
}

// 每帧都变的短串（倒计时数字）：逐字用缓存的纯暗影精灵，先画完暗影再直接 DrawString，避免每帧重建整串精灵
static float TextVolatile(Graphics& g, int role, const wchar_t* s, float x, float cy, const Color& fill) {
    const RoleFont& rf = g_rf[role];
    float top = std::round(cy + 0.36f * rf.px - rf.base);
    wchar_t one[2] = {};
    float cx = x, last = 0;
    for (const wchar_t* c = s; *c; c++) {
        bool cjk = IsCjk(*c);
        float gx = std::round(cx);
        if (*c != L' ') {
            one[0] = *c;
            const Sprite& sp = GetSprite(role, one, kNone, kNone);
            Blit(g, sp.bmp, gx - sp.pad, top - sp.pad, 1);
        }
        last = cjk ? rf.ctrack : rf.track;
        cx += Advance(role, cjk, *c) + last;
    }
    SetBrush(fill);
    cx = x;
    for (const wchar_t* c = s; *c; c++) {
        bool cjk = IsCjk(*c);
        if (*c != L' ')
            g.DrawString(c, 1, cjk ? rf.cjk : rf.lat, PointF(std::round(cx), top + rf.base - (cjk ? rf.ascC : rf.ascL)), g_fmt, g_brush);
        cx += Advance(role, cjk, *c) + (cjk ? rf.ctrack : rf.track);
    }
    return std::max(0.f, cx - x - last);
}

// ---------------------------------------------------------------- 文本内容

static void ResultText(const Result& r, wchar_t* buf, size_t n, Color& fill, Color& glow) {
    bool chance = r.action == Action::Blink || r.action == Action::Repulse;
    glow = kNone;
    switch (r.v) {
    case Verdict::Perfect:
        if (chance) wcscpy_s(buf, n, r.action == Action::Blink ? TR("闪现成功", "BLINK!") : TR("击退成功", "REPULSE!"));
        else swprintf(buf, n, L"%ls +%df", r.action == Action::Parry ? TR("完美弹反", "PERFECT PARRY") : TR("完美闪避", "PERFECT DODGE"),
                      Frames(r.lead));
        fill = kGoodText, glow = kGood;
        break;
    case Verdict::Early: swprintf(buf, n, TR("早 %df", "EARLY %df"), Frames(r.off)); fill = kEarly; break;
    case Verdict::Late: swprintf(buf, n, TR("晚 %df", "LATE %df"), Frames(r.off)); fill = kLate; break;
    case Verdict::Unhandled: wcscpy_s(buf, n, TR("未应对", "UNHANDLED")); fill = kMiss; break;
    default: wcscpy_s(buf, n, TR("方向/距离不对", "WRONG DIR / RANGE")); fill = kMiss; break;
    }
}

static const wchar_t* ActionText(NoteKind k) {
    switch (k) {
    case NoteKind::Parry: return TR("弹反", "PARRY");
    case NoteKind::Evade: return TR("完美闪避", "DODGE");
    case NoteKind::Danger: return TR("躲开", "AVOID");
    case NoteKind::Jump: return TR("跳跃", "JUMP");
    case NoteKind::Blink: return TR("前推+闪避", "FWD+DODGE");
    default: return TR("后拉+闪避", "BACK+DODGE");
    }
}

// 小徽标：细框 + 文字
static float Badge(Graphics& g, const wchar_t* s, float x, float cy, Align al, const Color& c, float sc, float alpha = 1,
                   bool snap = true) {
    float tw = TextWidth(kSmall, s), pad = 5 * sc, h = 15 * sc, w = tw + 2 * pad;
    if (al == kCenter) x -= w / 2;
    else if (al == kRight) x -= w;
    RoundPath(*g_tmp, x, cy - h / 2, w, h, 2 * sc);
    SetBrush(Al(Color(150, 10, 12, 16), alpha));
    g.FillPath(g_brush, g_tmp);
    SetPen(Al(c, 0.85f * alpha), std::max(1.f, sc));
    g.DrawPath(g_pen, g_tmp);
    Text(g, kSmall, s, x + pad, cy, kLeft, c, kNone, alpha, snap);
    return w;
}

// ---------------------------------------------------------------- 判定条

static const float kBarW = 420, kBarH = 124;
static const std::vector<Note> kNoNotes;

static void PaintBar(Graphics& g, const HudFrame& f, float s) {
    const float W = kBarW * s, lx0 = 16 * s, lx1 = W - 12 * s, ly = 48 * s, lh = 30 * s, cy = ly + lh / 2;
    const double hz = f.horizon > 0 ? f.horizon : 1.0;
    const float pps = (float)((lx1 - lx0) / hz);
    const float zr = lx0 + (float)std::clamp(f.window, 0.0, hz) * pps;
    const auto& notes = f.notes ? *f.notes : kNoNotes;
    const float rNear = 9 * s, rFar = 7 * s;

    // 最近的音符：t>=0 中最小的（蓝紫按开始算），正在覆盖判定线的蓝紫窗口算 0
    const Note *nearest = nullptr, *active = nullptr;
    auto key = [](const Note& n) { return std::max(0.0, n.tShow); };
    for (const Note& n : notes) {
        bool span = IsSpan(n.kind);
        if (span && n.tShow <= 0 && n.tShow + n.len > 0) { if (!active) active = &n; }
        else if (n.tShow < 0 || n.tShow > hz) continue;
        if (!nearest || key(n) < key(*nearest)) nearest = &n;
    }
    // 以“音符整个进入绿区”为准：右边缘也要过绿区右端，避免碰到边缘就按导致偏早
    bool lit = false;
    for (const Note& n : notes) {
        if (IsSpan(n.kind) || n.tShow < 0 || n.tShow > hz) continue;
        float r = &n == nearest ? rNear : rFar;
        if (lx0 + (float)n.tShow * pps + r <= zr) lit = true;
    }

    // 轨道底板
    Box(g, lx0, ly, lx1 - lx0, lh, Color(172, 10, 12, 16));
    for (int i = 1; i * 0.1 < hz - 1e-6; i++) {
        float x = std::floor(lx0 + (float)(i * 0.1) * pps) + 0.5f;
        bool major = i % 5 == 0;
        Line(g, x, ly + lh - (major ? 6 : 3) * s, x, ly + lh, Color(major ? 90 : 48, 237, 237, 237), 1);
    }

    // 绿区（完美窗口）：平时淡绿，有音符整个在里面时强烈点亮
    if (zr > lx0) {
        if (lit) {
            for (int i = 7; i >= 1; i--) {
                float e = i * 1.6f * s;
                Box(g, lx0, ly - e, zr - lx0 + e, lh + 2 * e, Al(kZoneLit, 0.055f));
            }
            Box(g, lx0, ly, zr - lx0, lh, Color(200, 110, 255, 160));
            Box(g, lx0, ly, zr - lx0, lh * 0.45f, Color(70, 255, 255, 255));
            Line(g, lx0, ly + 0.5f, zr, ly + 0.5f, Color(255, 255, 255, 255), std::max(1.f, 1.5f * s));
            Line(g, lx0, ly + lh - 0.5f, zr, ly + lh - 0.5f, Color(255, 255, 255, 255), std::max(1.f, 1.5f * s));
            Line(g, zr - 0.5f, ly - 5 * s, zr - 0.5f, ly + lh + 5 * s, Color(255, 255, 255, 255), std::max(1.f, 1.5f * s));
        } else {
            Box(g, lx0, ly, zr - lx0, lh, Color(58, 75, 227, 138));
            Line(g, lx0, ly + 0.5f, zr, ly + 0.5f, Al(kZone, 0.55f), 1);
            Line(g, lx0, ly + lh - 0.5f, zr, ly + lh - 0.5f, Al(kZone, 0.55f), 1);
            Line(g, zr - 0.5f, ly - 3 * s, zr - 0.5f, ly + lh + 3 * s, Al(kZone, 0.85f), std::max(1.f, 1.2f * s));
        }
    }

    // 轨道上下细线 + 右端角标
    Line(g, zr, ly + 0.5f, lx1, ly + 0.5f, Color(52, 237, 237, 237), 1);
    Line(g, zr, ly + lh - 0.5f, lx1, ly + lh - 0.5f, Color(52, 237, 237, 237), 1);
    {
        float bl = 7 * s, lw = std::max(1.f, 1.5f * s), x = lx1 - lw / 2;
        SetPen(Color(200, 237, 237, 237), lw);
        PointF a[] = {{x - bl, ly + lw / 2}, {x, ly + lw / 2}, {x, ly + bl}};
        PointF b[] = {{x - bl, ly + lh - lw / 2}, {x, ly + lh - lw / 2}, {x, ly + lh - bl}};
        g.DrawLines(g_pen, a, 3);
        g.DrawLines(g_pen, b, 3);
    }

    // 蓝紫窗口：从 t 到 t+len 的圆角条，判定线左侧裁掉
    float pulse = 0.5f + 0.5f * (float)std::sin(f.now * 6.2831853 * 3.0);
    g.SetClip(RectF(lx0, ly - 12 * s, lx1 - lx0, lh + 24 * s));
    for (const Note& n : notes) {
        if (!IsSpan(n.kind) || n.tShow > hz || n.tShow + n.len <= 0) continue;
        bool on = &n == active;
        Color c = KindColor(n.kind);
        float x0 = lx0 + (float)n.tShow * pps, x1 = std::min(lx1 + 20 * s, lx0 + (float)(n.tShow + n.len) * pps);
        float bh = 16 * s, by = cy - bh / 2;
        if (x1 - x0 < 1) x1 = x0 + 1;
        RoundPath(*g_tmp, x0, by, x1 - x0, bh, bh / 2);
        float ga = on ? 0.35f + 0.35f * pulse : 0.22f;
        SetPen(Al(c, ga * 0.5f), 10 * s);
        g.DrawPath(g_pen, g_tmp);
        SetPen(Al(c, ga), 5 * s);
        g.DrawPath(g_pen, g_tmp);
        SetBrush(on ? Mix(c, Color(255, 255, 255, 255), 0.06f + 0.14f * pulse) : Al(c, 0.9f));
        g.FillPath(g_brush, g_tmp);
        SetPen(Al(Color(255, 255, 255, 255), on ? 0.9f : 0.45f), std::max(1.f, 1.2f * s));
        g.DrawPath(g_pen, g_tmp);
        // 标签：双箭头 + 前闪/后闪（» 字形太小，自己画）
        float lx = std::max(x0, lx0) + 9 * s, ch = 3.2f * s, cw = 3 * s, lw = std::max(1.2f, 1.5f * s);
        SetPen(Color(255, 255, 255, 255), lw);
        for (int i = 0; i < 2; i++) {
            float ax = lx + i * 4.5f * s;
            PointF v[3];
            if (n.kind == NoteKind::Blink) v[0] = {ax, cy - ch}, v[1] = {ax + cw, cy}, v[2] = {ax, cy + ch};
            else v[0] = {ax + cw, cy - ch}, v[1] = {ax, cy}, v[2] = {ax + cw, cy + ch};
            g.DrawLines(g_pen, v, 3);
        }
        Text(g, kSmall, n.kind == NoteKind::Blink ? TR("前闪", "BLINK") : TR("后闪", "REPULSE"), lx + 13 * s, cy, kLeft,
             Color(255, 255, 255, 255));
    }
    g.ResetClip();

    // 判定线：蓝紫窗口覆盖时染成该颜色并发光
    {
        Color jc = active ? Mix(KindColor(active->kind), Color(255, 255, 255, 255), 0.35f) : kInk;
        float ext = 6 * s, gw = active ? 1 + pulse : 0.6f;
        if (active || lit) {
            Color gc = active ? KindColor(active->kind) : kZoneLit;
            Line(g, lx0, ly - ext, lx0, ly + lh + ext, Al(gc, 0.18f * gw), 10 * s);
            Line(g, lx0, ly - ext, lx0, ly + lh + ext, Al(gc, 0.35f * gw), 5 * s);
        } else {
            Line(g, lx0, ly - ext, lx0, ly + lh + ext, Color(60, 0, 0, 0), 5 * s);
        }
        Line(g, lx0, ly - ext, lx0, ly + lh + ext, jc, std::max(2.f, 2.5f * s));
        Line(g, lx0 - 4 * s, ly - ext, lx0 + 4 * s, ly - ext, jc, std::max(1.f, 1.5f * s));
        Line(g, lx0 - 4 * s, ly + lh + ext, lx0 + 4 * s, ly + lh + ext, jc, std::max(1.f, 1.5f * s));
    }

    // 点音符：远的先画，最近的在最上面
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
        const Note& n = *it;
        if (IsSpan(n.kind) || n.tShow < 0 || n.tShow > hz) continue;
        float r = &n == nearest ? rNear : rFar, x = lx0 + (float)n.tShow * pps;
        bool inZone = x + r <= zr;
        float fade = n.tShow > hz * 0.85 ? (float)((hz - n.tShow) / (hz * 0.15)) : 1.f;
        Color c = KindColor(n.kind);
        if (inZone) {
            Disc(g, x, cy, r + 7 * s, Color(50, 255, 255, 255));
            Disc(g, x, cy, r + 3.5f * s, Color(110, 255, 255, 255));
        } else {
            Disc(g, x, cy, r + 5 * s, Al(c, 0.14f * fade));
            Disc(g, x, cy, r + 2.5f * s, Al(c, 0.28f * fade));
        }
        Disc(g, x, cy, r, Al(c, fade));
        Disc(g, x - r * 0.3f, cy - r * 0.3f, r * 0.42f, Al(Color(90, 255, 255, 255), fade));
        if (inZone) SetPen(Color(255, 255, 255, 255), 2 * s);
        else SetPen(Al(Color(150, 0, 0, 0), fade), std::max(1.f, s));
        g.DrawEllipse(g_pen, x - r, cy - r, 2 * r, 2 * r);
    }

    // 信息行 / 状态行
    float iy = ly + lh + 17 * s;
    if (f.status) {
        Text(g, kInfo, f.status, W / 2, iy, kCenter, Color(225, 237, 237, 237));
    } else if (nearest) {
        wchar_t num[16], label[48], warn[64] = L"";
        swprintf(num, 16, L"%dms", Ms(key(*nearest)));
        swprintf(label, 48, L"  %ls", ActionText(nearest->kind));
        if (IsSpan(nearest->kind) && f.targetDistance > 0 && nearest->range > 0 && f.targetDistance > nearest->range)
            swprintf(warn, 64, TR("  太远 %.1fm>%.0fm", "  TOO FAR %.1fm>%.0fm"), std::min(f.targetDistance, 999.f), nearest->range);
        Color c = KindColor(nearest->kind);
        if (IsSpan(nearest->kind)) c = Mix(c, Color(255, 255, 255, 255), nearest->kind == NoteKind::Blink ? 0.35f : 0.22f); // 深色文字提亮
        float tr = g_rf[kInfo].track;
        float wn = TextWidth(kInfo, num) + tr, wl = TextWidth(kInfo, label), w1 = *warn ? TextWidth(kInfo, warn) + tr : 0;
        float x = std::round(W / 2 - (wn + wl + w1) / 2), w0 = wn + wl;
        TextVolatile(g, kInfo, num, x, iy, c);
        Text(g, kInfo, label, x + wn, iy, kLeft, c);
        if (*warn) Text(g, kInfo, warn, x + w0 + tr, iy, kLeft, kMiss);
    }

    // 判定结果弹字（1.2 秒）：弹入 + 淡出
    if (f.results && !f.results->empty()) {
        const Result& r = f.results->front();
        double age = (GetTickCount64() - r.shownAt) / 1000.0;
        if (age >= 0 && age < 1.2) {
            wchar_t buf[64];
            Color fill, glow;
            ResultText(r, buf, 64, fill, glow);
            float p = (float)std::min(1.0, age / 0.14), e = 1 - (1 - p) * (1 - p) * (1 - p);
            float k = 1.28f - 0.28f * e;
            float a = (float)(age < 0.05 ? age / 0.05 : age > 0.8 ? 1 - (age - 0.8) / 0.4 : 1.0);
            if (age < 0.16) fill = Mix(Color(255, 255, 255, 255), fill, std::floor((float)age / 0.04f) * 0.25f); // 分 4 档，少建精灵
            float tw = TextWidth(kPop, buf), bw = 0, gap = 7 * s;
            if (r.automatic) bw = TextWidth(kSmall, L"AUTO") + 10 * s + gap;
            float py = 22 * s + (1 - e) * 5 * s;
            // 弹入动画期间用变换缩放；落定后按整像素直接贴，省去重采样
            bool anim = e < 0.999f;
            GraphicsState st = g.Save();
            float x = -(tw + bw) / 2, yy = 0;
            if (anim) {
                g.TranslateTransform(W / 2, py);
                g.ScaleTransform(k, k);
            } else {
                x += W / 2, yy = py;
            }
            if (r.automatic) x += Badge(g, L"AUTO", x, yy, kLeft, kEarly, s, a, !anim) + gap;
            Text(g, kPop, buf, x, yy, kLeft, fill, glow, a, !anim);
            g.Restore(st);
        }
    }

    if (f.autoOn) {
        float x = lx1, y = 12 * s;
        float tw = Text(g, kSmall, L"AUTO", x, y, kRight, Al(kEarly, 0.9f));
        Disc(g, x - tw - 6 * s, y, 2.6f * s, Al(kEarly, 0.9f));
    }
}

// ---------------------------------------------------------------- 统计面板

static const float kPanelW = 300, kPanelH = 118;

static void PaintPanel(Graphics& g, const HudFrame& f, float s) {
    const float W = kPanelW * s, H = kPanelH * s, px = 14 * s;
    Plate(g, 0, 0, W, H, s);

    // 标题行
    float y = 16 * s;
    float x = px + Text(g, kSmall, L"SBPARRY", px, y, kLeft, kInk) + 12 * s;
    Color sc = f.connected ? kGood : kEarly;
    Disc(g, x, y, 3 * s, sc);
    Disc(g, x, y, 6 * s, Al(sc, 0.2f));
    Text(g, kSmall, f.connected ? TR("已连接", "CONNECTED") : TR("等待游戏…", "WAITING FOR GAME…"), x + 9 * s, y, kLeft, Al(sc, 0.95f));
    Line(g, px, 28 * s, W - px, 28 * s, Color(40, 237, 237, 237), 1);

    // 最近一次结果
    y = 45 * s;
    if (f.results && !f.results->empty()) {
        wchar_t buf[64];
        Color fill, glow;
        ResultText(f.results->front(), buf, 64, fill, glow);
        Text(g, kInfo, buf, px, y, kLeft, glow.GetA() ? kGood : fill);
        if (f.results->front().automatic) Badge(g, L"AUTO", W - px, y, kRight, kEarly, s);
    } else {
        Text(g, kInfo, TR("对着敌人攻击按格挡/闪避", "PARRY OR DODGE AN ATTACK"), px, y, kLeft, kDim);
    }

    // 最近 20 次统计
    int perfect = 0, early = 0, late = 0, cnt = 0;
    if (f.results)
        for (const Result& r : *f.results) {
            if (cnt >= 20) break;
            cnt++;
            if (r.v == Verdict::Perfect) perfect++;
            else if (r.v == Verdict::Early) early++;
            else if (r.v == Verdict::Late) late++;
        }
    wchar_t v[4][16];
    swprintf(v[0], 16, L"%d", perfect);
    swprintf(v[1], 16, L"%d", early);
    swprintf(v[2], 16, L"%d", late);
    if (cnt) swprintf(v[3], 16, L"%d%%", perfect * 100 / cnt);
    else wcscpy_s(v[3], L"--");
    const wchar_t* lab[4] = {TR("完美", "PERFECT"), TR("早", "EARLY"), TR("晚", "LATE"), TR("成功率", "RATE")};
    const Color col[4] = {kGood, kEarly, kLate, kInk};
    const float cx[4] = {px, px + 70 * s, px + 128 * s, px + 186 * s};
    for (int i = 0; i < 4; i++) {
        Text(g, kSmall, lab[i], cx[i], 64 * s, kLeft, kDim);
        Text(g, kInfo, v[i], cx[i], 81 * s, kLeft, col[i]);
    }
    wchar_t last[32];
    swprintf(last, 32, TR("最近 %d 次", "LAST %d"), cnt);
    Text(g, kSmall, last, W - px, 16 * s, kRight, kDim);

    // 底行：窗口 + 自动
    Line(g, px, 94 * s, W - px, 94 * s, Color(40, 237, 237, 237), 1);
    wchar_t win[64];
    swprintf(win, 64, TR("窗口 %dms · %df", "WINDOW %dms · %dF"), Ms(f.window), Frames(f.window));
    Text(g, kSmall, win, px, 106 * s, kLeft, kInk);
    Text(g, kSmall, f.autoOn ? TR("自动 开", "AUTO ON") : TR("自动 关", "AUTO OFF"), W - px, 106 * s, kRight,
         f.autoOn ? kEarly : kDim);
}

// ---------------------------------------------------------------- 提示

static SIZE ToastSize(const wchar_t* text, float s) {
    float w = TextWidth(kToast, text);
    return SIZE{(int)std::ceil(w + 64 * s), (int)std::ceil(52 * s)};
}
static void PaintToast(Graphics& g, const wchar_t* text, float s, int w, int h) {
    float m = 8 * s, ph = (float)h - 2 * m, pw = (float)w - 2 * m;
    RoundPath(*g_tmp, m, m, pw, ph, ph / 2);
    SetPen(Color(40, 0, 0, 0), 8 * s);
    g.DrawPath(g_pen, g_tmp);
    SetBrush(Color(180, 10, 12, 16));
    g.FillPath(g_brush, g_tmp);
    SetPen(Color(90, 237, 237, 237), 1);
    g.DrawPath(g_pen, g_tmp);
    Disc(g, m + 18 * s, h / 2.f, 2.5f * s, kInk);
    Disc(g, m + 18 * s, h / 2.f, 5 * s, Al(kInk, 0.18f));
    Text(g, kToast, text, m + 30 * s, h / 2.f, kLeft, kInk);
}

// ---------------------------------------------------------------- 分层窗口

struct Layer {
    HWND wnd = nullptr;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    Bitmap* gb = nullptr;
    int w = 0, h = 0;
    bool shown = false;
};
static Layer g_bar, g_panel, g_toast;
static HWND g_hotkey;
static std::wstring g_toastText;
static float g_toastScale;

static void FreeSurface(Layer& l) {
    delete l.gb;
    l.gb = nullptr;
    if (l.dc) { SelectObject(l.dc, l.old); DeleteDC(l.dc); l.dc = nullptr; }
    if (l.bmp) { DeleteObject(l.bmp); l.bmp = nullptr; }
    l.w = l.h = 0;
}
static bool Ensure(Layer& l, int w, int h) {
    if (l.bmp && l.w == w && l.h == h) return true;
    FreeSurface(l);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // 自上而下，和 GDI+ 的正跨距一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    l.bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &l.bits, nullptr, 0);
    if (!l.bmp) return false;
    l.dc = CreateCompatibleDC(nullptr);
    l.old = SelectObject(l.dc, l.bmp);
    l.gb = new Bitmap(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)l.bits);
    l.w = w, l.h = h;
    return true;
}
static void Setup(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetCompositingQuality(CompositingQualityHighSpeed);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
}
static void Present(Layer& l, int x, int y, BYTE alpha = 255) {
    POINT pos{x, y}, src{0, 0};
    SIZE sz{l.w, l.h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(l.wnd, nullptr, &pos, &sz, l.dc, &src, 0, &bf, ULW_ALPHA);
    if (!l.shown) {
        SetWindowPos(l.wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        l.shown = true;
    }
}
static void Hide(Layer& l) {
    if (l.shown) { ShowWindow(l.wnd, SW_HIDE); l.shown = false; }
}

static float CurScale() {
    float s = std::clamp(g_cfg.uiScale, 50, 300) / 100.f;
    if (s != g_scale) BuildFonts(s);
    return s;
}

static FontFamily* PickFamily(std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names) {
        auto* f = new FontFamily(n);
        if (f->GetLastStatus() == Ok && f->IsAvailable()) return f;
        delete f;
    }
    return FontFamily::GenericSansSerif()->Clone();
}

void OverlayInit(HINSTANCE inst, WNDPROC hotkeyProc) {
    GdiplusStartupInput in;
    GdiplusStartup(&g_gdip, &in, nullptr);
    g_pen = new Pen(Color(255, 255, 255, 255), 1);
    g_pen->SetLineJoin(LineJoinRound);
    g_brush = new SolidBrush(Color(255, 255, 255, 255));
    g_tmp = new GraphicsPath();
    g_fmt = StringFormat::GenericTypographic()->Clone();
    g_fmt->SetFormatFlags(g_fmt->GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
    g_measureBmp = new Bitmap(1, 1, PixelFormat32bppPARGB);
    g_measure = new Graphics(g_measureBmp);
    g_measure->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g_ia = new ImageAttributes();
    g_latB = PickFamily({L"Bahnschrift SemiBold", L"Segoe UI Semibold", L"Segoe UI"});
    g_cjk = PickFamily({L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimHei"});
    // 没有独立的半粗族时用加粗样式
    WCHAR name[LF_FACESIZE];
    g_latB->GetFamilyName(name);
    g_latBStyle = wcsstr(name, L"Semi") ? FontStyleRegular : FontStyleBold;
    g_scale = 0;
    CurScale();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"SBParryOverlay";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = hotkeyProc;
    wc.lpszClassName = L"SBParryHotkey";
    RegisterClassExW(&wc);
    const DWORD ex = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    for (Layer* l : {&g_bar, &g_panel, &g_toast})
        l->wnd = CreateWindowExW(ex, L"SBParryOverlay", L"SBParry", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    g_hotkey = CreateWindowExW(WS_EX_TOOLWINDOW, L"SBParryHotkey", L"SBParry", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
}

HWND OverlayHotkeyWindow() { return g_hotkey; }

SIZE OverlayBarSize() {
    float s = CurScale();
    return SIZE{(int)std::lround(kBarW * s), (int)std::lround(kBarH * s)};
}

void OverlayDrawBar(const HudFrame& f, int x, int y, bool visible) {
    if (!visible) { Hide(g_bar); return; }
    float s = CurScale();
    SIZE sz = OverlayBarSize();
    if (!Ensure(g_bar, sz.cx, sz.cy)) return;
    memset(g_bar.bits, 0, (size_t)sz.cx * sz.cy * 4);
    {
        Graphics g(g_bar.gb);
        Setup(g);
        PaintBar(g, f, s);
    }
    Present(g_bar, x, y);
}

void OverlayDrawPanel(const HudFrame& f, const RECT& screen, int corner, bool visible) {
    if (!visible) { Hide(g_panel); return; }
    float s = CurScale();
    int w = (int)std::lround(kPanelW * s), h = (int)std::lround(kPanelH * s), m = 24;
    if (!Ensure(g_panel, w, h)) return;
    memset(g_panel.bits, 0, (size_t)w * h * 4);
    {
        Graphics g(g_panel.gb);
        Setup(g);
        PaintPanel(g, f, s);
    }
    int x = (corner & 1) ? screen.right - w - m : screen.left + m;
    int y = (corner & 2) ? screen.bottom - h - m : screen.top + m;
    Present(g_panel, x, y);
}

void OverlayDrawToast(const HudFrame& f, const RECT& screen) {
    const double kLife = 1.6;
    if (!f.toast || !*f.toast || f.toastAge < 0 || f.toastAge >= kLife) { Hide(g_toast); return; }
    float s = CurScale();
    // 文字不变时只改整体透明度，不重画
    if (g_toastText != f.toast || g_toastScale != s || !g_toast.bmp) {
        SIZE sz = ToastSize(f.toast, s);
        if (!Ensure(g_toast, sz.cx, sz.cy)) return;
        memset(g_toast.bits, 0, (size_t)sz.cx * sz.cy * 4);
        {
            Graphics g(g_toast.gb);
            Setup(g);
            PaintToast(g, f.toast, s, sz.cx, sz.cy);
        }
        g_toastText = f.toast;
        g_toastScale = s;
    }
    double a = std::min({1.0, f.toastAge / 0.1, (kLife - f.toastAge) / 0.4});
    int x = (screen.left + screen.right - g_toast.w) / 2;
    int y = screen.top + (int)((screen.bottom - screen.top) * 0.22) - g_toast.h / 2;
    Present(g_toast, x, y, (BYTE)std::lround(std::clamp(a, 0.0, 1.0) * 255));
}

void OverlayShutdown() {
    for (Layer* l : {&g_bar, &g_panel, &g_toast}) {
        FreeSurface(*l);
        if (l->wnd) DestroyWindow(l->wnd);
        l->wnd = nullptr;
        l->shown = false;
    }
    if (g_hotkey) DestroyWindow(g_hotkey);
    g_hotkey = nullptr;
    if (!g_gdip) return;
    ClearTextCache();
    delete g_ia;
    g_ia = nullptr;
    for (RoleFont& rf : g_rf) {
        delete rf.lat;
        delete rf.cjk;
        rf = RoleFont{};
    }
    delete g_measure;
    delete g_measureBmp;
    delete g_fmt;
    delete g_tmp;
    delete g_brush;
    delete g_pen;
    delete g_latB;
    delete g_cjk;
    g_measure = nullptr, g_measureBmp = nullptr, g_fmt = nullptr, g_tmp = nullptr, g_brush = nullptr, g_pen = nullptr;
    g_latB = g_cjk = nullptr;
    g_scale = 0;
    g_toastText.clear();
    GdiplusShutdown(g_gdip);
    g_gdip = 0;
}

// ---------------------------------------------------------------- 预览/测试钩子（tools/overlay_preview 用，需先 OverlayInit）

static void RenderTo(Bitmap& bmp, void (*paint)(Graphics&, const HudFrame&, float), const HudFrame& f) {
    Graphics g(&bmp);
    g.Clear(Color(0, 0, 0, 0));
    Setup(g);
    paint(g, f, CurScale());
}
void OverlayRenderBarToBitmap(const HudFrame& f, Bitmap& bmp) { RenderTo(bmp, PaintBar, f); }
SIZE OverlayPanelSize() {
    float s = CurScale();
    return SIZE{(int)std::lround(kPanelW * s), (int)std::lround(kPanelH * s)};
}
void OverlayRenderPanelToBitmap(const HudFrame& f, Bitmap& bmp) { RenderTo(bmp, PaintPanel, f); }
SIZE OverlayToastSize(const HudFrame& f) { return ToastSize(f.toast ? f.toast : L"", CurScale()); }
void OverlayRenderToastToBitmap(const HudFrame& f, Bitmap& bmp) {
    Graphics g(&bmp);
    g.Clear(Color(0, 0, 0, 0));
    Setup(g);
    float s = CurScale();
    PaintToast(g, f.toast ? f.toast : L"", s, (int)bmp.GetWidth(), (int)bmp.GetHeight());
}
