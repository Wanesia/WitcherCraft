#include "red_engine.hpp"

#include "log.hpp"
#include "pattern.hpp"

#include <cstring>
#include <vector>

namespace wc::red
{
Offsets g;

namespace
{
using pattern::FindAll;
using pattern::FindFirst;
using pattern::MatchAt;
using pattern::Parse;
using pattern::Rel;

// One native registration in the 5.0 exe (e.g. inside the "export math natives" routine):
//   mov edx, 0F8h ; lea rcx,[rbp-10h] ; mov r8d,10h ; call Alloc
//   mov rdi,[rbp-10h] ; xor edx,edx ; mov r8,[rbp-8] ; mov rcx,rdi ; call memset
//   test rdi,rdi ; jz ... ; call GetNamePool
//   lea rdx, "Name" ; mov rcx,rax ; call CNamePool::AddEntry
//   lea r8, nativeFn ; mov [rbp+10h],eax ; lea rdx,[rbp+10h] ; mov rcx,rdi ; call CFunction::CFunction
//   mov rdi,rax ; jmp +3 ; mov rdi,rbx
//   call SRTTI::GetInstance ; mov rdx,rdi ; mov rcx,rax ; call CRTTISystem::RegisterGlobalFunction
constexpr const char* kRegBlock =
    "BA ?? ?? 00 00 48 8D 4D F0 41 B8 10 00 00 00 E8 ?? ?? ?? ?? 48 8B 7D F0 33 D2 4C 8B 45 F8 48 8B CF "
    "E8 ?? ?? ?? ?? 48 85 FF 74 ?? E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? "
    "4C 8D 05 ?? ?? ?? ?? 89 45 10 48 8D 55 10 48 8B CF E8 ?? ?? ?? ?? 48 8B F8 EB ?? 48 8B FB "
    "E8 ?? ?? ?? ?? 48 8B D7 48 8B C8 E8 ?? ?? ?? ??";
constexpr size_t kBlockLea = 0x30;       // lea rdx,"Name"
constexpr size_t kBlockAlloc = 0x0F;     // call Alloc
constexpr size_t kBlockMemset = 0x21;    // call memset
constexpr size_t kBlockNamePool = 0x2B;  // call GetNamePool
constexpr size_t kLeaAddName = 10;       // call AddEntry (relative to lea)
constexpr size_t kLeaFn = 15;            // lea r8, fn
constexpr size_t kLeaCtor = 32;          // call CFunction ctor
constexpr size_t kLeaRtti = 45;          // call SRTTI::GetInstance
constexpr size_t kLeaRegister = 56;      // call RegisterGlobalFunction  <-- hook site

// Looser shape shared by every native registration: lea rdx,name ; mov rcx,rax ; call AddEntry ; lea r8,fn
constexpr const char* kNameToFn = "48 8D 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ??";

// Inside CFunction::CFunction(global): mov rcx,[counterHolder] ; lea rdx,[nativeTable]
constexpr const char* kCtorTable = "48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ??";
constexpr const char* kCtorCounter = "8B 81 ?? ?? 00 00";    // mov eax,[rcx+counterOffset]
constexpr const char* kCtorIndexStore = "89 87 ?? ?? 00 00"; // mov [rdi+indexOffset],eax

// In StrLen native: mov r8d,1 ; call Realloc   /  test rcx,rcx ; jz +5 ; call Free
constexpr const char* kReallocCall = "41 B8 01 00 00 00 E8";
constexpr const char* kMemClass = "41 B9 ?? 00 00 00";
constexpr const char* kFreeCall = "48 85 C9 74 05 E8";

// The 5.0 reference build these offsets were measured on.
constexpr uint32_t kRefTimeDateStamp = 0x6ABD8695;

template <class T> T As(uint8_t* p)
{
    return reinterpret_cast<T>(p);
}

// All "\0name\0" occurrences in .rdata, returned as pointers to the first char.
std::vector<uint8_t*> FindCString(const pe::Module& m, const char* name)
{
    std::vector<uint8_t*> out;
    const pe::Section* rd = m.Find(".rdata");
    if (!rd)
        return out;
    std::string needle;
    needle.push_back('\0');
    needle += name;
    needle.push_back('\0');
    for (uint8_t* h : pattern::FindBytes(rd->begin, rd->size, needle.data(), needle.size()))
        out.push_back(h + 1);
    return out;
}

// All `lea rdx,[rip+X]` (48 8D 15) in .text whose target is one of `targets`.
std::vector<std::vector<uint8_t*>> FindLeaRdxXrefs(const pe::Module& m, const std::vector<std::vector<uint8_t*>>& targets)
{
    std::vector<std::vector<uint8_t*>> out(targets.size());
    const pe::Section* tx = m.Find(".text");
    if (!tx || tx->size < 8)
        return out;
    uint8_t* p = tx->begin;
    uint8_t* end = tx->begin + tx->size - 7;
    while (p < end)
    {
        p = static_cast<uint8_t*>(std::memchr(p, 0x48, static_cast<size_t>(end - p)));
        if (!p)
            break;
        if (p[1] == 0x8D && p[2] == 0x15)
        {
            uint8_t* t = Rel(p, 3, 7);
            for (size_t i = 0; i < targets.size(); ++i)
                for (uint8_t* s : targets[i])
                    if (t == s)
                        out[i].push_back(p);
        }
        ++p;
    }
    return out;
}

bool InText(const pe::Module& m, const void* p)
{
    return m.InSection(".text", p, 1);
}

uint8_t* FindIn(uint8_t* from, size_t len, const char* pat)
{
    return FindFirst(from, len, Parse(pat));
}
} // namespace

bool Resolve(const pe::Module& m, Offsets& o)
{
    o = Offsets{};
    const pe::Section* tx = m.Find(".text");
    if (!tx || !m.Find(".rdata") || !m.Find(".data"))
    {
        WLOG("resolve: missing sections");
        return false;
    }

    // 1) Anchor strings.
    const char* names[] = {"RandDifferent", "StrLen", "Log"};
    std::vector<std::vector<uint8_t*>> strs;
    for (const char* n : names)
    {
        strs.push_back(FindCString(m, n));
        WLOG("resolve: string \"%s\": %zu hit(s)", n, strs.back().size());
    }
    auto xrefs = FindLeaRdxXrefs(m, strs);

    // 2) The RandDifferent registration block -> all registration primitives + hook site.
    const auto regPat = Parse(kRegBlock);
    for (uint8_t* lea : xrefs[0])
    {
        uint8_t* blk = lea - kBlockLea;
        if (!m.In(blk, regPat.size()) || !MatchAt(blk, regPat))
            continue;
        o.regBlock = blk;
        break;
    }
    if (!o.regBlock)
    {
        WLOG("resolve: RandDifferent registration block NOT found (%zu lea xrefs)", xrefs[0].size());
        return false;
    }
    uint8_t* lea = o.regBlock + kBlockLea;
    std::memcpy(&o.cfunctionSize, o.regBlock + 1, 4);
    o.Alloc = As<decltype(o.Alloc)>(Rel(o.regBlock + kBlockAlloc, 1, 5));
    o.Memset = Rel(o.regBlock + kBlockMemset, 1, 5);
    o.GetNamePool = As<decltype(o.GetNamePool)>(Rel(o.regBlock + kBlockNamePool, 1, 5));
    o.AddName = As<decltype(o.AddName)>(Rel(lea + kLeaAddName, 1, 5));
    o.CFunctionCtor = As<decltype(o.CFunctionCtor)>(Rel(lea + kLeaCtor, 1, 5));
    o.GetRTTI = As<decltype(o.GetRTTI)>(Rel(lea + kLeaRtti, 1, 5));
    o.RegisterGlobalFunction = As<decltype(o.RegisterGlobalFunction)>(Rel(lea + kLeaRegister, 1, 5));
    o.hookCallSite = lea + kLeaRegister;
    uint8_t* randDifferentFn = Rel(lea + kLeaFn, 3, 7);

    const void* fns[] = {reinterpret_cast<void*>(o.Alloc), o.Memset, reinterpret_cast<void*>(o.GetNamePool),
                         reinterpret_cast<void*>(o.AddName), reinterpret_cast<void*>(o.CFunctionCtor),
                         reinterpret_cast<void*>(o.GetRTTI), reinterpret_cast<void*>(o.RegisterGlobalFunction),
                         randDifferentFn};
    for (const void* f : fns)
    {
        if (!InText(m, f))
        {
            WLOG("resolve: registration primitive %p outside .text", f);
            return false;
        }
    }
    if (o.cfunctionSize < 0x80 || o.cfunctionSize > 0x400)
    {
        WLOG("resolve: implausible CFunction size 0x%X", o.cfunctionSize);
        return false;
    }

    // 3) Native/opcode table + slot counter, read out of the CFunction ctor.
    {
        auto* ctor = reinterpret_cast<uint8_t*>(o.CFunctionCtor);
        if (uint8_t* p = FindIn(ctor, 0x100, kCtorTable))
        {
            o.counterHolderSlot = As<void**>(Rel(p, 3, 7));
            o.nativeTable = As<void**>(Rel(p + 7, 3, 7));
            if (uint8_t* c = FindIn(p, 0x40, kCtorCounter))
                std::memcpy(&o.counterOffset, c + 2, 4);
            if (uint8_t* c = FindIn(p, 0x60, kCtorIndexStore))
                std::memcpy(&o.functionIndexOffset, c + 2, 4);
            auto diff = reinterpret_cast<uintptr_t>(o.counterHolderSlot) - reinterpret_cast<uintptr_t>(o.nativeTable);
            if (o.counterHolderSlot > reinterpret_cast<void**>(o.nativeTable) && diff % 8 == 0 && diff / 8 >= 256 &&
                diff / 8 <= 65536)
                o.tableCapacity = static_cast<uint32_t>(diff / 8);
        }
        if (uint8_t* p = FindIn(randDifferentFn, 0x40, "4C 8D 3D ?? ?? ?? ??"))
            o.tableFromNative = Rel(p, 3, 7);
        if (!o.nativeTable || !m.InSection(".data", o.nativeTable, 8))
        {
            WLOG("resolve: native table not found in CFunction ctor");
            return false;
        }
        if (o.tableFromNative && o.tableFromNative != o.nativeTable)
            WLOG("resolve: WARNING native table mismatch ctor=%p native=%p", o.nativeTable, o.tableFromNative);
    }

    // 4) String Realloc/Free + memclass from the StrLen native.
    {
        const auto nameToFn = Parse(kNameToFn);
        for (uint8_t* l : xrefs[1])
        {
            if (!m.In(l, nameToFn.size()) || !MatchAt(l, nameToFn))
                continue;
            uint8_t* fn = Rel(l + kLeaFn, 3, 7);
            if (!InText(m, fn))
                continue;
            uint8_t* rc = FindIn(fn, 0x80, kReallocCall);
            uint8_t* fc = FindIn(fn, 0xC0, kFreeCall);
            if (!rc || !fc)
                continue;
            o.Realloc = As<decltype(o.Realloc)>(Rel(rc + 6, 1, 5));
            o.Free = As<decltype(o.Free)>(Rel(fc + 5, 1, 5));
            if (uint8_t* mc = FindIn(fn, 0x80, kMemClass))
                o.stringMemClass = mc[2];
            break;
        }
        if (!o.Realloc || !o.Free || !InText(m, reinterpret_cast<void*>(o.Realloc)) ||
            !InText(m, reinterpret_cast<void*>(o.Free)))
        {
            WLOG("resolve: string Realloc/Free not found (StrLen native)");
            return false;
        }
    }

    // 5) Log() sink slot (optional; only used by the fallback channel).
    {
        const auto nameToFn = Parse(kNameToFn);
        for (uint8_t* l : xrefs[2])
        {
            if (!m.In(l, nameToFn.size()) || !MatchAt(l, nameToFn))
                continue;
            uint8_t* fn = Rel(l + kLeaFn, 3, 7);
            if (!InText(m, fn))
                continue;
            if (uint8_t* c = FindIn(fn, 0xC0, "FF 15 ?? ?? ?? ??"))
            {
                auto** slot = As<void**>(Rel(c, 2, 6));
                if (m.InSection(".data", slot, 8))
                {
                    o.logSinkSlot = slot;
                    break;
                }
            }
        }
        if (!o.logSinkSlot)
            WLOG("resolve: Log() sink slot not found (fallback channel unavailable)");
    }
    return true;
}

void DumpOffsets(const pe::Module& m, const Offsets& o)
{
    auto r = [&](const void* p) { return static_cast<unsigned long long>(m.Rva(p)); };
    WLOG("offsets (RVA):");
    WLOG("  regBlock(RandDifferent)   %08llX", r(o.regBlock));
    WLOG("  hookCallSite              %08llX", r(o.hookCallSite));
    WLOG("  Alloc                     %08llX", r(reinterpret_cast<void*>(o.Alloc)));
    WLOG("  Memset                    %08llX", r(o.Memset));
    WLOG("  GetNamePool               %08llX", r(reinterpret_cast<void*>(o.GetNamePool)));
    WLOG("  CNamePool::AddEntry       %08llX", r(reinterpret_cast<void*>(o.AddName)));
    WLOG("  CFunction::CFunction      %08llX", r(reinterpret_cast<void*>(o.CFunctionCtor)));
    WLOG("  SRTTI::GetInstance        %08llX", r(reinterpret_cast<void*>(o.GetRTTI)));
    WLOG("  RegisterGlobalFunction    %08llX", r(reinterpret_cast<void*>(o.RegisterGlobalFunction)));
    WLOG("  sizeof(CFunction)         0x%X", o.cfunctionSize);
    WLOG("  nativeTable               %08llX (cross-check %08llX)", r(o.nativeTable), r(o.tableFromNative));
    WLOG("  counterHolderSlot         %08llX (+0x%X), CFunction index @+0x%X, capacity %u", r(o.counterHolderSlot),
         o.counterOffset, o.functionIndexOffset, o.tableCapacity);
    WLOG("  Realloc                   %08llX (memclass 0x%X)", r(reinterpret_cast<void*>(o.Realloc)),
         o.stringMemClass);
    WLOG("  Free                      %08llX", r(reinterpret_cast<void*>(o.Free)));
    WLOG("  Log sink slot             %08llX", r(o.logSinkSlot));
}

int CompareWithReference(const pe::Module& m, const Offsets& o)
{
    if (m.timeDateStamp != kRefTimeDateStamp)
        return -1;
    struct
    {
        const char* name;
        const void* p;
        uintptr_t rva;
    } refs[] = {
        {"hookCallSite", o.hookCallSite, 0x27AB79D},
        {"Alloc", reinterpret_cast<void*>(o.Alloc), 0x0BCD5D0},
        {"Memset", o.Memset, 0x28C34F0},
        {"GetNamePool", reinterpret_cast<void*>(o.GetNamePool), 0x031D930},
        {"AddName", reinterpret_cast<void*>(o.AddName), 0x26B0E00},
        {"CFunctionCtor", reinterpret_cast<void*>(o.CFunctionCtor), 0x26F5810},
        {"GetRTTI", reinterpret_cast<void*>(o.GetRTTI), 0x031FCF0},
        {"RegisterGlobalFunction", reinterpret_cast<void*>(o.RegisterGlobalFunction), 0x26C5F30},
        {"nativeTable", o.nativeTable, 0x5A6BA80},
        {"counterHolderSlot", o.counterHolderSlot, 0x5A73A80},
        {"Realloc", reinterpret_cast<void*>(o.Realloc), 0x031E220},
        {"Free", reinterpret_cast<void*>(o.Free), 0x0BCC930},
        {"logSinkSlot", o.logSinkSlot, 0x4051A20},
    };
    int bad = 0;
    for (auto& e : refs)
    {
        if (m.Rva(e.p) != e.rva)
        {
            ++bad;
            WLOG("  reference mismatch: %s = %08llX, expected %08llX", e.name,
                 static_cast<unsigned long long>(m.Rva(e.p)), static_cast<unsigned long long>(e.rva));
        }
    }
    return bad;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
void ReadParam(CScriptStackFrame* s, void* out)
{
    uint8_t*& code = Code(s);
    const uint8_t op = *code;
    ++code;
    auto fn = reinterpret_cast<ScriptNative>(g.nativeTable[op]);
    if (fn)
        fn(Context(s), s, out);
}

std::string ReadStringParam(CScriptStackFrame* s)
{
    alignas(16) uint8_t buf[32]{}; // RedString + slack
    ReadParam(s, buf);
    auto* rs = reinterpret_cast<RedString*>(buf);
    std::string out;
    if (rs->data && rs->size)
        out.assign(rs->data, strnlen(rs->data, rs->size));
    if (rs->data)
        g.Free(rs->data);
    return out;
}

void AssignString(void* result, std::string_view v)
{
    auto* dst = static_cast<RedString*>(result);
    if (!dst)
        return;
    if (v.empty())
    {
        dst->size = 0; // mirrors the engine: keeps the buffer, size 0
        return;
    }
    const auto n = static_cast<uint32_t>(v.size() + 1);
    void* p = g.Realloc(dst->data, n, 1, g.stringMemClass);
    if (!p)
        return;
    std::memcpy(p, v.data(), v.size());
    static_cast<char*>(p)[v.size()] = 0;
    dst->data = static_cast<char*>(p);
    dst->size = n;
}

int NativeSlotsUsed()
{
    if (!g.counterHolderSlot || !g.counterOffset)
        return -1;
    auto* holder = static_cast<uint8_t*>(*g.counterHolderSlot);
    if (!holder)
        return -1;
    return static_cast<int>(*reinterpret_cast<uint32_t*>(holder + g.counterOffset));
}

bool RegisterGlobalNative(const char* staticName, ScriptNative fn)
{
    const int used = NativeSlotsUsed();
    if (g.tableCapacity && used >= 0 && static_cast<uint32_t>(used) + 1 >= g.tableCapacity)
    {
        WLOG("register %s: native table full (%d/%u) - skipped", staticName, used, g.tableCapacity);
        return false;
    }

    AllocResult ar{};
    g.Alloc(&ar, g.cfunctionSize, 16);
    if (!ar.ptr)
    {
        WLOG("register %s: Alloc failed", staticName);
        return false;
    }
    std::memset(ar.ptr, 0, ar.size >= g.cfunctionSize ? ar.size : g.cfunctionSize);

    // NB: the name pool may keep the pointer -> pass a string with static storage.
    const uint32_t cname = g.AddName(g.GetNamePool(), staticName);
    void* func = g.CFunctionCtor(ar.ptr, &cname, fn);
    g.RegisterGlobalFunction(g.GetRTTI(), func);

    uint32_t slot = 0xFFFFFFFF;
    if (g.functionIndexOffset)
        slot = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(func) + g.functionIndexOffset);
    const bool slotOk = slot != 0xFFFFFFFF && (!g.tableCapacity || slot < g.tableCapacity) &&
                        g.nativeTable[slot] == reinterpret_cast<void*>(fn);
    WLOG("register %-22s CName=%u CFunction=%p slot=%u table[slot]==fn:%s (slots used before: %d)", staticName, cname,
         func, slot, slotOk ? "yes" : "NO", used);
    return true;
}
} // namespace wc::red
