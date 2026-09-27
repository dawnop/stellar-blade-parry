#include "ue.h"
#include <unordered_map>

namespace ue {

// UE 4.26 布局
static const uint64_t kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
static const uint64_t kStructSuper = 0x40, kStructChildProps = 0x50, kStructSize = 0x58;
static const uint64_t kFieldClass = 0x08, kFieldNext = 0x20, kFieldName = 0x28;
static const uint64_t kPropElemSize = 0x3C, kPropOffset = 0x4C, kPropInner = 0x78; // Array.Inner / Struct.Struct
static const uint64_t kBoolByteOffset = 0x79, kBoolFieldMask = 0x7B;
static const uint64_t kTableRowStruct = 0x28, kTableRowMap = 0x30;
static const int kChunk = 64 * 1024, kItemSize = 0x18;
static const int32_t kItemPendingKill = 1 << 29, kItemUnreachable = 1 << 28;

static HANDLE g_h;
static uint64_t g_objArray, g_namePool;
static std::unordered_map<uint32_t, std::string> g_names;
static std::vector<Obj> g_objs;
static std::map<std::pair<uint64_t, uint64_t>, bool> g_isA;
// 名字池已经扫过的位置（按字符串找名字用；池只增不减）
static uint32_t g_scanBlock, g_scanCursor;
static std::unordered_map<std::string, uint32_t> g_nameIdx;

bool Open(DWORD pid, uint64_t objArray, uint64_t namePool) {
    Close();
    g_h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    g_objArray = objArray, g_namePool = namePool;
    return g_h != nullptr;
}

void Close() {
    if (g_h) CloseHandle(g_h);
    g_h = nullptr;
    g_names.clear();
    g_objs.clear();
    g_isA.clear();
    g_nameIdx.clear();
    g_scanBlock = g_scanCursor = 0;
}

bool Read(uint64_t a, void* p, size_t n) {
    SIZE_T got = 0;
    return g_h && a && ReadProcessMemory(g_h, (LPCVOID)a, p, n, &got) && got == n;
}

// ---------------------------------------------------------------- 名字

static std::string Utf8(const wchar_t* w, int n) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
    return s;
}

// 名字池：Blocks[] 在 +0x10，FName 序号高 16 位是块号、低 16 位 ×2 是块内偏移；
// 条目头 2 字节：bit0 宽字符，高 10 位长度
std::string Name(uint32_t idx, uint32_t num) {
    auto it = g_names.find(idx);
    if (it == g_names.end()) {
        std::string s;
        uint64_t blk = R<uint64_t>(g_namePool + 0x10 + (uint64_t)(idx >> 16) * 8);
        uint8_t b[2 + 2 * 1024];
        if (blk && Read(blk + (idx & 0xFFFF) * 2ull, b, 2)) {
            uint16_t hdr = (uint16_t)(b[0] | b[1] << 8);
            int len = hdr >> 6;
            bool wide = hdr & 1;
            if (len > 0 && Read(blk + (idx & 0xFFFF) * 2ull + 2, b + 2, (size_t)len * (wide ? 2 : 1)))
                s = wide ? Utf8((const wchar_t*)(b + 2), len) : std::string((const char*)b + 2, len);
        }
        it = g_names.emplace(idx, s).first;
    }
    return num ? it->second + "_" + std::to_string(num - 1) : it->second;
}

// 按字符串找名字：名字池有上百万个名字，只记要找的那些。池只增不减，从上次扫到的位置接着往后扫；
// 第一次问到的名字要从头扫一遍
static uint32_t ScanNames(uint32_t& block, uint32_t& cursor) {
    uint32_t curBlock = R<uint32_t>(g_namePool + 8), curCursor = R<uint32_t>(g_namePool + 12);
    std::vector<uint8_t> buf;
    uint32_t found = 0;
    for (; block <= curBlock && block < 8192; block++, cursor = 0) {
        uint32_t end = block == curBlock ? curCursor : 0x20000; // 每块 128KB（2 字节对齐 × 64K）
        if (end > 0x20000) break;
        if (cursor < end) {
            uint64_t blk = R<uint64_t>(g_namePool + 0x10 + block * 8ull);
            buf.assign(end, 0);
            if (!blk || !Read(blk, buf.data(), end)) break;
            uint32_t p = cursor;
            while (p + 2 <= end) {
                uint16_t hdr = (uint16_t)(buf[p] | buf[p + 1] << 8);
                int len = hdr >> 6;
                if (!len) break; // 块尾
                bool wide = hdr & 1;
                uint32_t bytes = 2 + len * (wide ? 2 : 1);
                if (p + bytes > end) break;
                if (!wide) {
                    auto it = g_nameIdx.find(std::string((const char*)&buf[p + 2], len));
                    if (it != g_nameIdx.end() && !it->second) it->second = (block << 16) | (p / 2), found++;
                }
                p += (bytes + 1) & ~1u;
            }
            cursor = p;
        }
        if (block == curBlock) break;
    }
    return found;
}

void WantNames(const std::vector<std::string>& names) {
    for (const std::string& s : names) g_nameIdx.emplace(s, 0);
    ScanNames(g_scanBlock, g_scanCursor);
}

uint32_t FindName(const std::string& s) {
    auto it = g_nameIdx.find(s);
    if (it == g_nameIdx.end()) {
        // 没登记过的：从头扫一遍（慢，尽量用 WantNames 预先登记）
        g_nameIdx.emplace(s, 0);
        uint32_t b = 0, c = 0;
        ScanNames(b, c);
    } else if (!it->second) {
        ScanNames(g_scanBlock, g_scanCursor); // 登记过但还没有：看看新加的名字
    }
    return g_nameIdx[s];
}

// FString = TArray<wchar_t>（数据地址、个数含结尾 0）
static std::string FStringAt(uint64_t data, int32_t num) {
    if (num <= 1 || num > 65536) return "";
    std::wstring w(num, L'\0');
    return Read(data, w.data(), num * 2ull) ? Utf8(w.c_str(), num - 1) : "";
}

std::string ReadFString(uint64_t at) {
    struct { uint64_t data; int32_t num, max; } a{};
    return Read(at, &a, sizeof(a)) ? FStringAt(a.data, a.num) : "";
}

std::string Row::Str(const Prop& p) const { return p ? FStringAt(Get<uint64_t>(p.off), Get<int32_t>(p.off + 8)) : ""; }

// ---------------------------------------------------------------- 对象

// FChunkedFixedUObjectArray 在 GUObjectArray+0x10：Objects（块指针数组）、NumElements +0x14、NumChunks +0x1C。
// 每个槽 FUObjectItem（0x18）：对象指针 +0、Flags +8。槽里的对象换了才重读对象头
void RefreshObjects() {
    uint64_t arr = g_objArray + 0x10;
    uint64_t chunks = R<uint64_t>(arr);
    int num = R<int32_t>(arr + 0x14), nchunks = R<int32_t>(arr + 0x1C);
    if (!chunks || num <= 0 || num > 8 * 1024 * 1024 || nchunks <= 0 || nchunks > 256) return;
    g_objs.resize(num);
    std::vector<uint8_t> buf;
    for (int c = 0; c < nchunks && c * kChunk < num; c++) {
        int n = std::min(kChunk, num - c * kChunk);
        uint64_t cp = R<uint64_t>(chunks + c * 8ull);
        buf.resize((size_t)n * kItemSize);
        if (!cp || !Read(cp, buf.data(), buf.size())) continue;
        for (int i = 0; i < n; i++) {
            Obj& o = g_objs[(size_t)c * kChunk + i];
            uint64_t p;
            memcpy(&p, &buf[(size_t)i * kItemSize], 8);
            memcpy(&o.itemFlags, &buf[(size_t)i * kItemSize + 8], 4);
            if (!p) { o = Obj{}; continue; }
            if (p == o.addr) continue;
            uint8_t h[0x28];
            if (!Read(p, h, sizeof(h))) { o = Obj{}; continue; }
            o.addr = p;
            memcpy(&o.flags, h + 8, 4);
            memcpy(&o.cls, h + kObjClass, 8);
            memcpy(&o.name, h + kObjName, 4);
            memcpy(&o.num, h + kObjName + 4, 4);
            memcpy(&o.outer, h + kObjOuter, 8);
        }
    }
}

const std::vector<Obj>& Objects() { return g_objs; }

std::string ObjName(uint64_t obj) {
    uint32_t n[2] = {};
    return obj && Read(obj + kObjName, n, 8) ? Name(n[0], n[1]) : "";
}

uint64_t Outer(uint64_t obj) { return obj ? R<uint64_t>(obj + kObjOuter) : 0; }

uint64_t FindObject(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return 0;
    std::string pkg = path.substr(0, dot), name = path.substr(dot + 1);
    uint32_t ni = FindName(name), pi = FindName(pkg);
    if (!ni || !pi) return 0;
    for (const Obj& o : g_objs)
        if (o.addr && o.name == ni && !o.num && o.outer && R<uint32_t>(o.outer + kObjName) == pi && !R<uint64_t>(o.outer + kObjOuter))
            return o.addr;
    return 0;
}

bool IsA(uint64_t cls, uint64_t base) {
    if (!cls || !base) return false;
    auto key = std::make_pair(cls, base);
    auto it = g_isA.find(key);
    if (it != g_isA.end()) return it->second;
    bool r = false;
    for (uint64_t c = cls; c && !r; c = R<uint64_t>(c + kStructSuper)) r = c == base;
    g_isA[key] = r;
    return r;
}

static bool Live(const Obj& o) {
    return o.addr && !(o.flags & (RF_ClassDefaultObject | RF_ArchetypeObject)) && !(o.itemFlags & (kItemPendingKill | kItemUnreachable));
}

std::vector<uint64_t> Instances(uint64_t cls) {
    std::vector<uint64_t> out;
    for (const Obj& o : g_objs)
        if (Live(o) && IsA(o.cls, cls)) out.push_back(o.addr);
    return out;
}

uint64_t FirstInstance(uint64_t cls) {
    for (const Obj& o : g_objs)
        if (Live(o) && IsA(o.cls, cls)) return o.addr;
    return 0;
}

// ---------------------------------------------------------------- 字段

Prop FindProp(uint64_t strct, const std::string& name) {
    Prop p;
    for (uint64_t s = strct; s; s = R<uint64_t>(s + kStructSuper)) {
        int guard = 0;
        for (uint64_t f = R<uint64_t>(s + kStructChildProps); f && guard++ < 4096; f = R<uint64_t>(f + kFieldNext)) {
            uint32_t n[2] = {};
            if (!Read(f + kFieldName, n, 8) || Name(n[0], n[1]) != name) continue;
            uint64_t fc = R<uint64_t>(f + kFieldClass);
            p.type = fc ? Name(R<uint32_t>(fc)) : "";
            p.off = R<int32_t>(f + kPropOffset);
            p.size = R<int32_t>(f + kPropElemSize);
            if (p.type == "BoolProperty") p.boolByte = R<uint8_t>(f + kBoolByteOffset), p.boolMask = R<uint8_t>(f + kBoolFieldMask);
            if (p.type == "ArrayProperty" || p.type == "StructProperty") p.inner = R<uint64_t>(f + kPropInner);
            return p;
        }
    }
    return p;
}

Prop FindProp(const std::string& structPath, const std::string& name) { return FindProp(FindObject(structPath), name); }

bool ReadTable(const std::string& path, Table& t) {
    t = Table{};
    t.addr = FindObject(path);
    if (!t.addr) return false;
    t.rowStruct = R<uint64_t>(t.addr + kTableRowStruct);
    t.rowSize = R<int32_t>(t.rowStruct + kStructSize);
    // RowMap：TSparseArray 的元素 {FName, uint8* 行, 哈希} 0x18 字节，按数组顺序
    uint64_t data = R<uint64_t>(t.addr + kTableRowMap);
    int num = R<int32_t>(t.addr + kTableRowMap + 8);
    if (!t.rowStruct || t.rowSize <= 0 || !data || num <= 0 || num > 200000) return false;
    std::vector<uint8_t> buf((size_t)num * 0x18);
    if (!Read(data, buf.data(), buf.size())) return false;
    t.rows.reserve(num);
    for (int i = 0; i < num; i++) {
        uint32_t n[2];
        uint64_t row;
        memcpy(n, &buf[(size_t)i * 0x18], 8);
        memcpy(&row, &buf[(size_t)i * 0x18 + 8], 8);
        t.rows.emplace_back(Name(n[0], n[1]), row);
    }
    return true;
}

} // namespace ue
