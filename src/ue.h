// 在本进程里直接读游戏的 UE 反射信息（不依赖 UE4SS）：名字池、对象数组、类 / 结构体字段、数据表。
// 只在导出线程（native.cpp）里用，有自己的进程句柄。布局是 UE 4.26 的标准布局（和 UE4SS 的 MemberVariableLayout 一致）
#pragma once
#include "common.h"

namespace ue {

// 对象标记
enum : uint32_t { RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20 };

struct Obj {
    uint64_t addr = 0, cls = 0, outer = 0;
    uint32_t name = 0, num = 0; // FName
    uint32_t flags = 0;         // UObject::ObjectFlags
    int32_t itemFlags = 0;      // FUObjectItem::Flags（PendingKill / Unreachable）
};

// 一个字段（FProperty）
struct Prop {
    int off = -1;          // Offset_Internal；-1=没找到
    int size = 0;          // ElementSize
    std::string type;      // FFieldClass 名：BoolProperty / FloatProperty / NameProperty / StrProperty / ArrayProperty ...
    uint8_t boolByte = 0, boolMask = 0xFF; // BoolProperty：ByteOffset / FieldMask
    uint64_t inner = 0;    // ArrayProperty：元素的 FProperty；StructProperty：UScriptStruct
    explicit operator bool() const { return off >= 0; }
};

bool Open(DWORD pid, uint64_t objArray, uint64_t namePool);
void Close();
bool Read(uint64_t a, void* p, size_t n);
template <class T> T R(uint64_t a) {
    T v{};
    Read(a, &v, sizeof(T));
    return v;
}

std::string Name(uint32_t idx, uint32_t num = 0); // FName -> 字符串（Number 非 0 时加 _N-1 后缀）
// 名字池里按字符串找 FName 序号（只看已有的名字，不新建）；找不到返回 0。
// 要找的名字先用 WantNames 一起登记，扫一遍名字池就都找到
void WantNames(const std::vector<std::string>& names);
uint32_t FindName(const std::string& s);
std::string ReadFString(uint64_t at); // at 处的 FString（TArray<wchar_t>）

void RefreshObjects(); // 读对象数组（增量：只重读变了的槽）
const std::vector<Obj>& Objects();
std::string ObjName(uint64_t obj);
uint64_t Outer(uint64_t obj);
// 按路径找对象："/Script/Engine.PlayerController"、"/Game/Local/Data/EffectTable.EffectTable"
uint64_t FindObject(const std::string& path);
bool IsA(uint64_t cls, uint64_t base);
// 类 cls 的非默认实例（按对象数组顺序）
std::vector<uint64_t> Instances(uint64_t cls);
uint64_t FirstInstance(uint64_t cls);

Prop FindProp(uint64_t strct, const std::string& name); // 连同父类一起找
Prop FindProp(const std::string& structPath, const std::string& name);

// 数据表：行结构体、按 RowMap 顺序的 (行名, 行地址)
struct Table {
    uint64_t addr = 0, rowStruct = 0;
    int rowSize = 0;
    std::vector<std::pair<std::string, uint64_t>> rows;
};
bool ReadTable(const std::string& path, Table& t);

// 一行数据读进缓冲区后按字段取值
struct Row {
    std::vector<uint8_t> b;
    bool Load(uint64_t addr, int size) {
        b.assign(size, 0);
        return size > 0 && Read(addr, b.data(), b.size());
    }
    template <class T> T Get(int off) const {
        T v{};
        if (off >= 0 && off + sizeof(T) <= b.size()) memcpy(&v, &b[off], sizeof(T));
        return v;
    }
    bool Bool(const Prop& p) const { return p && (Get<uint8_t>(p.off + p.boolByte) & p.boolMask) != 0; }
    float Float(const Prop& p) const { return p ? Get<float>(p.off) : 0.f; }
    int Int(const Prop& p) const { return !p ? 0 : p.size == 1 ? Get<uint8_t>(p.off) : Get<int32_t>(p.off); }
    std::string FName(const Prop& p) const { return p ? Name(Get<uint32_t>(p.off), Get<uint32_t>(p.off + 4)) : "None"; }
    std::string Str(const Prop& p) const;
    // TArray：数据地址和个数
    uint64_t ArrData(const Prop& p) const { return p ? Get<uint64_t>(p.off) : 0; }
    int ArrNum(const Prop& p) const { return p ? Get<int32_t>(p.off + 8) : 0; }
};

} // namespace ue
