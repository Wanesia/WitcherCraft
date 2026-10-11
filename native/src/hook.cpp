#include "hook.hpp"

#include "log.hpp"
#include "pattern.hpp"

#include <Windows.h>
#include <cstring>

namespace wc::hook
{
namespace
{
constexpr intptr_t kMaxRel = 0x7FF00000;

struct StubPage
{
    uint8_t* page = nullptr;
    size_t used = 0;
    size_t size = 0;
};
StubPage g_pages[4];

bool Reachable(const void* from, const void* to)
{
    intptr_t d = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from);
    return d > -kMaxRel && d < kMaxRel;
}

uint8_t* TryAllocAt(uintptr_t addr, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)))
        return nullptr;
    if (mbi.State != MEM_FREE || mbi.RegionSize < size)
        return nullptr;
    return static_cast<uint8_t*>(
        VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
}

uint8_t* AllocNear(const void* nearAddr, size_t size)
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t center = reinterpret_cast<uintptr_t>(nearAddr) & ~(gran - 1);
    const uintptr_t lo = center > static_cast<uintptr_t>(kMaxRel) ? center - kMaxRel : gran;
    const uintptr_t hi = center + kMaxRel;

    // Walk outward from the target, alternating below/above.
    for (uintptr_t off = gran; off < static_cast<uintptr_t>(kMaxRel); off += gran)
    {
        if (center >= lo + off)
            if (auto* p = TryAllocAt(center - off, size))
                return p;
        if (center + off < hi)
            if (auto* p = TryAllocAt(center + off, size))
                return p;
    }
    return nullptr;
}
} // namespace

void* MakeNearJump(void* nearAddr, void* target)
{
    constexpr size_t kStub = 16;
    StubPage* sp = nullptr;
    for (auto& p : g_pages)
    {
        if (p.page && Reachable(nearAddr, p.page) && p.used + kStub <= p.size)
        {
            sp = &p;
            break;
        }
    }
    if (!sp)
    {
        for (auto& p : g_pages)
        {
            if (!p.page)
            {
                p.page = AllocNear(nearAddr, 0x1000);
                p.size = p.page ? 0x1000 : 0;
                p.used = 0;
                if (p.page)
                    sp = &p;
                break;
            }
        }
    }
    if (!sp)
    {
        WLOG("hook: could not allocate a stub page near %p", nearAddr);
        return nullptr;
    }

    uint8_t* stub = sp->page + sp->used;
    sp->used += kStub;
    // FF 25 00 00 00 00   jmp qword ptr [rip+0]
    // <8 bytes absolute target>
    stub[0] = 0xFF;
    stub[1] = 0x25;
    std::memset(stub + 2, 0, 4);
    auto t = reinterpret_cast<uint64_t>(target);
    std::memcpy(stub + 6, &t, 8);
    stub[14] = 0xCC;
    stub[15] = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), stub, kStub);
    return stub;
}

bool RedirectCall(uint8_t* callInsn, void* newTarget, void** oldTarget)
{
    if (!callInsn || callInsn[0] != 0xE8)
    {
        WLOG("hook: %p is not an E8 call", callInsn);
        return false;
    }
    uint8_t* orig = pattern::Rel(callInsn, 1, 5);

    void* stub = MakeNearJump(callInsn, newTarget);
    if (!stub)
        return false;

    intptr_t rel = reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(callInsn + 5);
    if (rel <= -kMaxRel || rel >= kMaxRel)
    {
        WLOG("hook: stub %p out of rel32 range from %p", stub, callInsn);
        return false;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(callInsn, 5, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        WLOG("hook: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    auto r32 = static_cast<int32_t>(rel);
    std::memcpy(callInsn + 1, &r32, 4);
    DWORD tmp = 0;
    VirtualProtect(callInsn, 5, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), callInsn, 5);

    if (oldTarget)
        *oldTarget = orig;
    return true;
}
} // namespace wc::hook
